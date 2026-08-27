// MicroPatterns device provisioning over Web Bluetooth.
//
// Sends WiFi credentials and the user ID to an ESP32 device (M5Paper / Watchy)
// over a small custom GATT service, so nothing has to be baked into firmware.
//
// Wire format (see docs/analysis/device-provisioning-design.md):
//   editor -> device   {"cmd":"provision","userId":"...","networks":[{"ssid":"...","psk":"..."}]}
//                      {"cmd":"status"}
//                      {"cmd":"forget"}
//   device -> editor   JSON reply
// Messages are newline-terminated in BOTH directions and chunked, because the
// negotiated MTU is around 185 bytes and a three-network payload is bigger.

const SERVICE_UUID = '6d70726f-7669-7369-6f6e-000000000001';
const WRITE_CHAR_UUID = '6d70726f-7669-7369-6f6e-000000000002';
const NOTIFY_CHAR_UUID = '6d70726f-7669-7369-6f6e-000000000003';

// Conservative payload size for a ~185 byte MTU (ATT overhead is 3 bytes, and
// some stacks negotiate lower). The device reassembles until it sees "\n".
const CHUNK_SIZE = 180;
const NEWLINE = 0x0a;

// Reply timeouts, in ms. Provisioning writes NVS, so it gets longer.
const TIMEOUT_DEFAULT = 10000;
const TIMEOUT_PROVISION = 20000;

const MAX_NETWORKS = 5; // matches the NVS cap in the design doc
const LOCAL_STORAGE_NETWORKS_KEY = 'micropatterns_provisioning_ssids';

// Same alphabet as the editor's user IDs (nanoid customAlphabet, length 10).
const ID_ALPHABET = '123456789bcdfghjkmnpqrstvwxyz';
const ID_LENGTH = 10;

// ---------------------------------------------------------------------------
// Environment detection
// ---------------------------------------------------------------------------

function isIOS() {
    const ua = navigator.userAgent || '';
    if (/iPad|iPhone|iPod/.test(ua)) return true;
    // iPadOS 13+ reports a desktop Safari UA; touch points give it away.
    if (/Macintosh/.test(ua) && typeof navigator.maxTouchPoints === 'number' && navigator.maxTouchPoints > 1) {
        return true;
    }
    return false;
}

/**
 * Returns {ok, reason, message} describing whether Web Bluetooth can work here.
 * Never returns ok:true unless navigator.bluetooth actually exists.
 */
export function detectBluetoothSupport() {
    const fileProtocol = window.location.protocol === 'file:';
    const secure = window.isSecureContext === true;

    if (isIOS()) {
        return {
            ok: false,
            reason: 'ios',
            message: 'No browser on iPhone or iPad can do this. That includes Chrome, Edge and Firefox on iOS, ' +
                'which are all Safari underneath and inherit the same limit — installing another browser will not help. ' +
                'Provision the device from Chrome or Edge on a computer, or from Chrome on Android.'
        };
    }

    if (!('bluetooth' in navigator)) {
        if (fileProtocol) {
            return {
                ok: false,
                reason: 'file',
                message: 'This page was opened from a file:// path, which is not a secure context, so Web Bluetooth ' +
                    'is switched off. Serve the folder over http://localhost or use the deployed HTTPS editor.'
            };
        }
        if (!secure) {
            return {
                ok: false,
                reason: 'insecure',
                message: 'Web Bluetooth needs HTTPS (or http://localhost). This page is not a secure context, ' +
                    'so the browser hides the API.'
            };
        }
        return {
            ok: false,
            reason: 'browser',
            message: 'Connecting a device needs Chrome or Edge on a computer, or Chrome on Android. ' +
                'Safari and Firefox do not implement Web Bluetooth on any platform.'
        };
    }

    // The API exists. It can still be unusable — that is reported at click time,
    // where we can tell "Bluetooth is off" apart from "you closed the chooser".
    if (fileProtocol || !secure) {
        return {
            ok: false,
            reason: 'insecure',
            message: 'Web Bluetooth exists in this browser but needs a secure context. Serve this page over ' +
                'HTTPS or http://localhost — a file:// copy silently lacks both HTTPS and a usable permission prompt.'
        };
    }

    return { ok: true, reason: 'supported', message: '' };
}

// ---------------------------------------------------------------------------
// Transport: chunked, newline-delimited JSON over GATT
// ---------------------------------------------------------------------------

class BleProvisioningClient {
    constructor(onLog) {
        this.onLog = onLog || (() => { });
        this.device = null;
        this.server = null;
        this.writeChar = null;
        this.notifyChar = null;
        // Reassembly buffer: raw bytes, so a multi-byte UTF-8 character split
        // across two notifications is never decoded as garbage.
        this.rxBytes = [];
        this.pending = null; // { resolve, reject, timer }
        this._onNotify = this._onNotify.bind(this);
        this._onDisconnect = this._onDisconnect.bind(this);
    }

    get connected() {
        return !!(this.device && this.device.gatt && this.device.gatt.connected && this.writeChar);
    }

    async connect({ acceptAllDevices = false } = {}) {
        const options = acceptAllDevices
            ? { acceptAllDevices: true, optionalServices: [SERVICE_UUID] }
            : { filters: [{ services: [SERVICE_UUID] }], optionalServices: [SERVICE_UUID] };

        this.device = await navigator.bluetooth.requestDevice(options);
        this.device.addEventListener('gattserverdisconnected', this._onDisconnect);

        this.onLog(`Connecting to "${this.device.name || 'unnamed device'}"...`);
        this.server = await this.device.gatt.connect();
        const service = await this.server.getPrimaryService(SERVICE_UUID);
        this.writeChar = await service.getCharacteristic(WRITE_CHAR_UUID);
        this.notifyChar = await service.getCharacteristic(NOTIFY_CHAR_UUID);

        this.rxBytes = [];
        await this.notifyChar.startNotifications();
        this.notifyChar.addEventListener('characteristicvaluechanged', this._onNotify);
        this.onLog('Connected.');
        return this.device.name || 'device';
    }

    disconnect() {
        this._failPending(new Error('Disconnected.'));
        if (this.device && this.device.gatt && this.device.gatt.connected) {
            this.device.gatt.disconnect();
        }
    }

    _onDisconnect() {
        this.writeChar = null;
        this.notifyChar = null;
        this.server = null;
        this.rxBytes = [];
        this._failPending(new Error('The device disconnected.'));
        this.onLog('Disconnected.');
        if (this.onDisconnected) this.onDisconnected();
    }

    _failPending(err) {
        if (!this.pending) return;
        const p = this.pending;
        this.pending = null;
        clearTimeout(p.timer);
        p.reject(err);
    }

    // Accumulate notification chunks and cut messages on "\n".
    _onNotify(event) {
        const view = event.target.value;
        for (let i = 0; i < view.byteLength; i++) {
            const byte = view.getUint8(i);
            if (byte === NEWLINE) {
                const bytes = Uint8Array.from(this.rxBytes);
                this.rxBytes = [];
                this._deliver(new TextDecoder().decode(bytes).trim());
            } else if (byte !== 0x0d) { // tolerate CRLF
                this.rxBytes.push(byte);
            }
        }
        // Guard against a device that never sends a terminator.
        if (this.rxBytes.length > 8192) {
            this.rxBytes = [];
            this._failPending(new Error('Reply was too long and had no newline terminator.'));
        }
    }

    _deliver(text) {
        if (!text) return; // keep-alive newline
        let parsed;
        try {
            parsed = JSON.parse(text);
        } catch (e) {
            this.onLog(`Ignored a non-JSON reply: ${text.slice(0, 120)}`);
            return;
        }
        if (!this.pending) {
            this.onLog(`Unsolicited message: ${text.slice(0, 200)}`);
            return;
        }
        const p = this.pending;
        this.pending = null;
        clearTimeout(p.timer);
        p.resolve(parsed);
    }

    async _write(bytes) {
        // Prefer a response-bearing write when the characteristic offers it:
        // it gives us flow control, so long payloads do not overrun the device.
        const props = this.writeChar.properties || {};
        for (let offset = 0; offset < bytes.length; offset += CHUNK_SIZE) {
            const chunk = bytes.slice(offset, offset + CHUNK_SIZE);
            if (props.write || !props.writeWithoutResponse) {
                await this.writeChar.writeValueWithResponse(chunk);
            } else {
                await this.writeChar.writeValueWithoutResponse(chunk);
            }
        }
    }

    /**
     * Send one command object and await its JSON reply.
     * The payload is JSON + "\n", split into <= CHUNK_SIZE byte writes.
     */
    async request(command, timeoutMs = TIMEOUT_DEFAULT) {
        if (!this.connected) throw new Error('Not connected to a device.');
        if (this.pending) throw new Error('A command is already in flight.');

        const bytes = new TextEncoder().encode(JSON.stringify(command) + '\n');
        const chunks = Math.ceil(bytes.length / CHUNK_SIZE);

        const reply = new Promise((resolve, reject) => {
            this.pending = {
                resolve,
                reject,
                timer: setTimeout(() => {
                    this.pending = null;
                    this.rxBytes = [];
                    reject(new Error(
                        'The device did not answer. It may not be in its provisioning window — ' +
                        'press the device button to open it (about 60 seconds) and try again.'
                    ));
                }, timeoutMs)
            };
        });

        this.onLog(`> ${command.cmd} (${bytes.length} bytes in ${chunks} chunk${chunks === 1 ? '' : 's'})`);
        try {
            await this._write(bytes);
        } catch (e) {
            this._failPending(e);
            throw e;
        }
        return reply;
    }
}

// ---------------------------------------------------------------------------
// Error classification
// ---------------------------------------------------------------------------

async function describeConnectError(err) {
    const name = err && err.name;
    if (name === 'NotFoundError') {
        // Chrome uses NotFoundError both for "user closed the chooser" and for
        // "nothing matched". Neither is a failure worth alarming about.
        let available = true;
        try {
            if (navigator.bluetooth.getAvailability) {
                available = await navigator.bluetooth.getAvailability();
            }
        } catch (e) { /* availability is advisory only */ }
        if (!available) {
            return 'Bluetooth looks switched off. Turn it on in your system settings, then try again.';
        }
        return 'No device was chosen. Either you closed the picker, or the device is not advertising yet — ' +
            'press its button to open the provisioning window, then try again. ' +
            'If it is advertising under a different name, tick "Show all Bluetooth devices".';
    }
    if (name === 'SecurityError') {
        return 'The browser blocked the request. This page must be served over HTTPS (or http://localhost).';
    }
    if (name === 'NotAllowedError') {
        return 'Permission was refused. The connect button must be clicked directly — the browser will not ' +
            'open the device picker without a user gesture.';
    }
    if (name === 'NetworkError') {
        return 'Could not connect to the device. It may have moved out of range, gone to sleep, or closed its ' +
            'provisioning window. Press its button and try again.';
    }
    if (name === 'NotSupportedError') {
        return 'The device does not expose the MicroPatterns provisioning service. Check that it is running ' +
            'firmware with provisioning support.';
    }
    return (err && err.message) ? err.message : String(err);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

function randomUserId() {
    const out = new Array(ID_LENGTH);
    const random = new Uint32Array(ID_LENGTH);
    crypto.getRandomValues(random);
    for (let i = 0; i < ID_LENGTH; i++) {
        out[i] = ID_ALPHABET[random[i] % ID_ALPHABET.length];
    }
    return out.join('');
}

/**
 * Wire up the provisioning panel.
 * @param {object} opts
 * @param {() => string} opts.getUserId   read the editor's current user ID
 * @param {(id: string) => void} opts.setUserId  write a rotated ID back to the editor
 * @param {() => string} [opts.generateUserId]   the editor's own ID generator
 */
export function initProvisioning(opts = {}) {
    const panel = document.getElementById('provisioningPanel');
    if (!panel) return;

    const noticeEl = document.getElementById('provNotice');
    const bodyEl = document.getElementById('provBody');
    const userIdInput = document.getElementById('provUserId');
    const rotateButton = document.getElementById('provRotateIdButton');
    const networkList = document.getElementById('provNetworkList');
    const addNetworkButton = document.getElementById('provAddNetworkButton');
    const connectButton = document.getElementById('provConnectButton');
    const disconnectButton = document.getElementById('provDisconnectButton');
    const provisionButton = document.getElementById('provSendButton');
    const statusButton = document.getElementById('provStatusButton');
    const forgetButton = document.getElementById('provForgetButton');
    const diagButton = document.getElementById('provDiagButton');
    const showAllToggle = document.getElementById('provShowAllDevices');
    const logEl = document.getElementById('provLog');

    const getUserId = opts.getUserId || (() => '');
    const setUserId = opts.setUserId || (() => { });
    const generateUserId = opts.generateUserId || randomUserId;

    function log(line) {
        if (!logEl) return;
        const stamp = new Date().toLocaleTimeString();
        logEl.textContent = `[${stamp}] ${line}\n` + logEl.textContent;
    }

    // --- support gate ------------------------------------------------------
    const support = detectBluetoothSupport();
    if (!support.ok) {
        // Rule 1 of the design doc: never render a control that cannot work.
        if (bodyEl) bodyEl.remove();
        if (noticeEl) {
            noticeEl.className = 'prov-notice prov-notice-blocked';
            noticeEl.textContent = support.message;
        }
        return;
    }
    if (noticeEl) {
        noticeEl.className = 'prov-notice';
        noticeEl.textContent = 'Works in Chrome or Edge on a computer, and Chrome on Android. ' +
            'Press the button on the device first: it only accepts provisioning for about 60 seconds afterwards.';
    }

    // Advisory check — the API can exist while the adapter is off.
    if (navigator.bluetooth.getAvailability) {
        navigator.bluetooth.getAvailability().then((available) => {
            if (!available) log('Bluetooth appears to be switched off. Turn it on in your system settings.');
        }).catch(() => { });
    }

    // --- user ID -----------------------------------------------------------
    userIdInput.value = getUserId() || '';

    userIdInput.addEventListener('input', () => {
        setUserId(userIdInput.value.trim());
    });

    rotateButton.addEventListener('click', () => {
        const fresh = generateUserId();
        userIdInput.value = fresh;
        setUserId(fresh);
        log(`Generated a fresh user ID. Provision the device to apply it, and make sure your scripts are saved under the new ID.`);
    });

    // --- ordered network list ----------------------------------------------
    // SSIDs and their order are remembered locally; passwords never are.
    let networks = [];
    try {
        const stored = JSON.parse(localStorage.getItem(LOCAL_STORAGE_NETWORKS_KEY) || '[]');
        if (Array.isArray(stored)) {
            networks = stored.filter((s) => typeof s === 'string').slice(0, MAX_NETWORKS).map((ssid) => ({ ssid, psk: '' }));
        }
    } catch (e) { /* ignore malformed storage */ }
    if (networks.length === 0) networks.push({ ssid: '', psk: '' });

    function persistSsids() {
        try {
            localStorage.setItem(
                LOCAL_STORAGE_NETWORKS_KEY,
                JSON.stringify(networks.map((n) => n.ssid).filter((s) => s !== ''))
            );
        } catch (e) { /* storage may be unavailable */ }
    }

    function renderNetworks() {
        networkList.textContent = '';
        networks.forEach((net, index) => {
            const row = document.createElement('div');
            row.className = 'prov-network-row';

            const rank = document.createElement('span');
            rank.className = 'prov-network-rank';
            rank.textContent = `${index + 1}.`;
            row.appendChild(rank);

            const ssid = document.createElement('input');
            ssid.type = 'text';
            ssid.className = 'prov-ssid';
            ssid.placeholder = 'Network name (SSID)';
            ssid.value = net.ssid;
            ssid.autocomplete = 'off';
            ssid.addEventListener('input', () => {
                net.ssid = ssid.value;
                persistSsids();
            });
            row.appendChild(ssid);

            const psk = document.createElement('input');
            psk.type = 'password';
            psk.className = 'prov-psk';
            psk.placeholder = 'Password';
            psk.value = net.psk;
            psk.autocomplete = 'new-password';
            psk.addEventListener('input', () => { net.psk = psk.value; });
            row.appendChild(psk);

            const buttons = document.createElement('div');
            buttons.className = 'prov-network-buttons';

            const up = document.createElement('button');
            up.type = 'button';
            up.className = 'secondary';
            up.title = 'Try this network earlier';
            up.textContent = '↑';
            up.disabled = index === 0;
            up.addEventListener('click', () => move(index, -1));
            buttons.appendChild(up);

            const down = document.createElement('button');
            down.type = 'button';
            down.className = 'secondary';
            down.title = 'Try this network later';
            down.textContent = '↓';
            down.disabled = index === networks.length - 1;
            down.addEventListener('click', () => move(index, 1));
            buttons.appendChild(down);

            const remove = document.createElement('button');
            remove.type = 'button';
            remove.className = 'secondary';
            remove.title = 'Remove this network';
            remove.textContent = '✕';
            remove.addEventListener('click', () => {
                networks.splice(index, 1);
                if (networks.length === 0) networks.push({ ssid: '', psk: '' });
                persistSsids();
                renderNetworks();
            });
            buttons.appendChild(remove);

            row.appendChild(buttons);
            networkList.appendChild(row);
        });
        addNetworkButton.disabled = networks.length >= MAX_NETWORKS;
    }

    function move(index, delta) {
        const target = index + delta;
        if (target < 0 || target >= networks.length) return;
        const [item] = networks.splice(index, 1);
        networks.splice(target, 0, item);
        persistSsids();
        renderNetworks();
    }

    addNetworkButton.addEventListener('click', () => {
        if (networks.length >= MAX_NETWORKS) return;
        networks.push({ ssid: '', psk: '' });
        persistSsids();
        renderNetworks();
    });

    renderNetworks();

    // --- connection --------------------------------------------------------
    const client = new BleProvisioningClient(log);
    client.onDisconnected = () => setConnectedUi(false);

    function setConnectedUi(connected) {
        connectButton.disabled = connected;
        disconnectButton.disabled = !connected;
        provisionButton.disabled = !connected;
        statusButton.disabled = !connected;
        forgetButton.disabled = !connected;
    }
    setConnectedUi(false);

    function setBusy(busy) {
        [connectButton, disconnectButton, provisionButton, statusButton, forgetButton]
            .forEach((b) => { if (busy) b.disabled = true; });
        if (!busy) setConnectedUi(client.connected);
    }

    connectButton.addEventListener('click', async () => {
        setBusy(true);
        try {
            const name = await client.connect({ acceptAllDevices: showAllToggle.checked });
            log(`Ready to talk to ${name}.`);
            setConnectedUi(true);
        } catch (err) {
            log(await describeConnectError(err));
            setConnectedUi(false);
        } finally {
            setBusy(false);
        }
    });

    disconnectButton.addEventListener('click', () => {
        client.disconnect();
        setConnectedUi(false);
    });

    function describeReply(reply) {
        if (reply && reply.ok === false) {
            return `Device reported an error: ${reply.error || 'unknown'}`;
        }
        const parts = [];
        // status returns the CURRENT SSID only — stored passwords are never readable back.
        if (reply.ssid) parts.push(`network "${reply.ssid}"`);
        if (typeof reply.rssi === 'number') parts.push(`${reply.rssi} dBm`);
        if (reply.userId) parts.push(`user ID ${reply.userId}`);
        if (reply.version) parts.push(`firmware ${reply.version}`);
        if (typeof reply.count === 'number') parts.push(`${reply.count} network(s) stored`);
        // Firmware v1 reports the stored SSIDs (names only, never passwords)
        // and which one last connected, so the user can confirm both the list
        // and its order arrived intact.
        if (Array.isArray(reply.ssids) && reply.ssids.length) {
            parts.push(`stored: ${reply.ssids.join(' > ')}`);
        }
        if (typeof reply.lastGood === 'number' && reply.lastGood >= 0) {
            parts.push(`last connected #${reply.lastGood + 1}`);
        }
        if (reply.provisioned === false) parts.push('not provisioned yet');
        if (typeof reply.networks === 'number') parts.push(`${reply.networks} network(s) written`);
        if (reply.forgotten) parts.push('credentials wiped');
        return parts.length ? parts.join(', ') : JSON.stringify(reply);
    }

    async function send(command, timeout) {
        setBusy(true);
        try {
            const reply = await client.request(command, timeout);
            log(`< ${describeReply(reply)}`);
            return reply;
        } catch (err) {
            log(await describeConnectError(err));
            return null;
        } finally {
            setBusy(false);
        }
    }

    provisionButton.addEventListener('click', async () => {
        const userId = userIdInput.value.trim();
        if (!userId) {
            log('Enter a user ID first, or generate one.');
            return;
        }
        const payload = networks
            .map((n) => ({ ssid: n.ssid.trim(), psk: n.psk }))
            .filter((n) => n.ssid !== '');
        if (payload.length === 0) {
            log('Add at least one network with a name.');
            return;
        }
        log(`Sending ${payload.length} network(s) in order: ${payload.map((n) => n.ssid).join(' -> ')}`);
        const reply = await send({ cmd: 'provision', userId, networks: payload }, TIMEOUT_PROVISION);
        if (reply && reply.ok !== false) {
            log('Device provisioned. Passwords are stored on the device and are never read back.');
        }
    });

    // Raw diagnostics. Prints the device's reply verbatim rather than through

    // describeReply(), because when provisioning fails the exact fields are

    // the point -- and on the Watchy, whose serial emits nothing, this is the

    // only way to see NVS state at all.

    if (diagButton) {

        diagButton.addEventListener('click', async () => {

            setBusy(true);

            try {

                const reply = await client.request({ cmd: 'diag' }, 10000);

                log('< diag ' + JSON.stringify(reply));

            } catch (err) {

                log(await describeConnectError(err));

            } finally {

                setBusy(false);

            }

        });

    }


    statusButton.addEventListener('click', () => send({ cmd: 'status' }));

    forgetButton.addEventListener('click', async () => {
        if (!window.confirm('Wipe the WiFi credentials and user ID stored on the device?')) return;
        const reply = await send({ cmd: 'forget' });
        if (reply && reply.ok !== false) log('Device credentials wiped.');
    });
}
