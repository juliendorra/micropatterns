// MicroPatterns device provisioning over Web Bluetooth.
//
// Sends WiFi credentials and the user ID to an ESP32 device (M5Paper / Watchy)
// over a small custom GATT service, so nothing has to be baked into firmware.
//
// Wire format (see docs/analysis/device-provisioning-design.md):
//   editor -> device   {"cmd":"provision","userId":"...","networks":[{"ssid":"...","psk":"..."}]}
//                      {"cmd":"status"}
//                      {"cmd":"forget"}
//                      {"cmd":"diag"}
//   device -> editor   JSON reply
//
// "provision" semantics, as the firmware implements them today:
//   * userId and networks are both OPTIONAL, but at least one must be present.
//     userId alone rotates the ID and leaves the networks alone; networks alone
//     replaces the list and leaves the ID alone. Neither is an error.
//   * "networks":[] is REFUSED — clearing is what "forget" is for — so an empty
//     list must never be sent; omit the key instead.
//   * A network sent with an empty psk means "keep the password already stored
//     for that SSID". The device matches by SSID and carries the stored password
//     forward; with no stored match the password really is blank (open network).
//     This is what makes it safe to resend the list after a page reload, since
//     passwords are deliberately never persisted in the browser.
//   * Reply: {"ok":true,"networks":N,"keptPasswords":K,"blankPasswords":B}
//     or on failure {"ok":false,"error":"...","stored":N}
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

    // Always filtered by the provisioning service: the picker then lists only
    // devices that can actually be provisioned, instead of every radio nearby.
    async connect() {
        this.device = await navigator.bluetooth.requestDevice({
            filters: [{ services: [SERVICE_UUID] }],
            optionalServices: [SERVICE_UUID]
        });
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
            'press its button to open the provisioning window, then try again. The picker only lists ' +
            'devices offering the MicroPatterns provisioning service, so a device missing from it is ' +
            'not advertising.';
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
    const sendUserIdToggle = document.getElementById('provSendUserId');
    const sendNetworksToggle = document.getElementById('provSendNetworks');
    const networkList = document.getElementById('provNetworkList');
    const addNetworkButton = document.getElementById('provAddNetworkButton');
    const connectButton = document.getElementById('provConnectButton');
    const disconnectButton = document.getElementById('provDisconnectButton');
    const provisionButton = document.getElementById('provSendButton');
    const statusButton = document.getElementById('provStatusButton');
    const forgetButton = document.getElementById('provForgetButton');
    const diagButton = document.getElementById('provDiagButton');
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
        if (sendUserIdToggle) sendUserIdToggle.checked = true;
        updateSendUi();
        log('Generated a fresh user ID. Send it to the device to apply it, and make sure your scripts are ' +
            'saved under the new ID. Untick "Write the WiFi networks" to rotate the ID on its own — the ' +
            'stored WiFi passwords are then left untouched.');
    });

    // --- what a "Send" will actually write ----------------------------------
    // The two parts of a provision command are independent on the device, so the
    // form says out loud which of them this click would change.
    function wantsUserId() {
        return !sendUserIdToggle || sendUserIdToggle.checked;
    }
    function wantsNetworks() {
        return !sendNetworksToggle || sendNetworksToggle.checked;
    }

    function updateSendUi() {
        const id = wantsUserId();
        const nets = wantsNetworks();
        if (provisionButton) {
            provisionButton.textContent = id && nets
                ? 'Send ID + networks'
                : (id ? 'Send user ID only' : (nets ? 'Send networks only' : 'Nothing selected'));
            provisionButton.title = id && nets
                ? 'Write the user ID and replace the stored network list'
                : (id ? 'Rotate the user ID; the stored networks and passwords are left untouched'
                    : (nets ? 'Replace the stored network list; the user ID is left untouched'
                        : 'Tick the user ID, the networks, or both'));
        }
        [[sendUserIdToggle, id], [sendNetworksToggle, nets]].forEach(([toggle, on]) => {
            if (!toggle) return;
            const section = toggle.closest('.prov-section');
            if (section) section.classList.toggle('prov-section-off', !on);
        });
        setConnectedUi(client.connected);
    }

    if (sendUserIdToggle) sendUserIdToggle.addEventListener('change', updateSendUi);
    if (sendNetworksToggle) sendNetworksToggle.addEventListener('change', updateSendUi);

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

    // SSIDs the device told us it already stores, from the last status/provision
    // reply. Names only — the device never reveals a password. Until we have
    // heard from a device this stays null, meaning "we cannot yet tell whether a
    // blank password would be kept or would genuinely be blank".
    let deviceSsids = null;

    function deviceKnows(ssid) {
        return !!(deviceSsids && ssid && deviceSsids.indexOf(ssid) !== -1);
    }

    function persistSsids() {
        try {
            localStorage.setItem(
                LOCAL_STORAGE_NETWORKS_KEY,
                JSON.stringify(networks.map((n) => n.ssid).filter((s) => s !== ''))
            );
        } catch (e) { /* storage may be unavailable */ }
    }

    // Per-row "what does a blank password mean here" updaters, so a fresh status
    // reply can refresh the notes without re-rendering (and stealing focus).
    let noteUpdaters = [];

    function refreshNetworkNotes() {
        noteUpdaters.forEach((update) => update());
    }

    function renderNetworks() {
        networkList.textContent = '';
        noteUpdaters = [];
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
            psk.title = 'Leave blank to keep the password already stored on the device for this name.';
            psk.value = net.psk;
            psk.autocomplete = 'new-password';
            row.appendChild(psk);

            // What an empty password field means depends on whether the device
            // already stores this SSID, so spell it out per row instead of
            // letting a blank field read as a forgotten one.
            const note = document.createElement('p');
            note.className = 'prov-network-note';

            function updateNote() {
                const name = net.ssid.trim();
                note.classList.remove('prov-network-note-warn');
                if (!name) {
                    note.textContent = 'Empty row — it will not be sent.';
                    return;
                }
                if (net.psk !== '') {
                    note.textContent = deviceKnows(name)
                        ? `Replaces the password stored on the device for "${name}".`
                        : `Sends a new password for "${name}".`;
                    return;
                }
                if (deviceKnows(name)) {
                    note.textContent = `Blank: the device keeps the password it already stores for "${name}".`;
                    return;
                }
                note.classList.add('prov-network-note-warn');
                note.textContent = deviceSsids
                    ? `Blank, and the device does not store "${name}" — it will be saved as an open network with no password.`
                    : `Blank: the device keeps its stored password for "${name}" if it has one, otherwise this is saved ` +
                    'as an open network. Use "Check status" to see which names the device already has.';
            }

            ssid.addEventListener('input', updateNote);
            psk.addEventListener('input', () => { net.psk = psk.value; updateNote(); });
            updateNote();
            noteUpdaters.push(updateNote);

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
            row.appendChild(note);
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
        // A provision with neither part ticked is refused by the device, so the
        // button is simply not offered in that state.
        provisionButton.disabled = !connected || (!wantsUserId() && !wantsNetworks());
        statusButton.disabled = !connected;
        forgetButton.disabled = !connected;
        if (diagButton) diagButton.disabled = !connected;
    }
    setConnectedUi(false);
    updateSendUi();

    function setBusy(busy) {
        [connectButton, disconnectButton, provisionButton, statusButton, forgetButton, diagButton]
            .forEach((b) => { if (busy && b) b.disabled = true; });
        if (!busy) setConnectedUi(client.connected);
    }

    connectButton.addEventListener('click', async () => {
        setBusy(true);
        let connectedNow = false;
        try {
            const name = await client.connect();
            log(`Ready to talk to ${name}.`);
            setConnectedUi(true);
            connectedNow = true;
        } catch (err) {
            log(await describeConnectError(err));
            setConnectedUi(false);
        } finally {
            setBusy(false);
        }
        // Read the stored state immediately: without the device's SSID list we
        // cannot tell the user whether a blank password field would be kept or
        // would be written as an open network.
        if (connectedNow) await send({ cmd: 'status' });
    });

    disconnectButton.addEventListener('click', () => {
        client.disconnect();
        setConnectedUi(false);
    });

    // The next device may store something else entirely, so stop claiming to
    // know what a blank password would do once we are no longer connected.
    const clearDeviceSsids = () => { deviceSsids = null; refreshNetworkNotes(); };
    const previousOnDisconnected = client.onDisconnected;
    client.onDisconnected = () => { previousOnDisconnected(); clearDeviceSsids(); };
    disconnectButton.addEventListener('click', clearDeviceSsids);

    // Remember the SSIDs the device reports, so the form can say whether a blank
    // password field would be kept or would genuinely be blank.
    function noteDeviceSsids(reply) {
        if (!reply) return;
        if (Array.isArray(reply.ssids)) {
            deviceSsids = reply.ssids.filter((s) => typeof s === 'string');
            refreshNetworkNotes();
        }
    }

    function describeReply(reply) {
        if (reply && reply.ok === false) {
            let line = `Device reported an error: ${reply.error || 'unknown'}`;
            // "stored" says how many networks survived the refusal, which is the
            // reassuring half of a failed provision.
            if (typeof reply.stored === 'number') {
                line += ` (${reply.stored} network(s) still stored on the device)`;
            }
            return line;
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
        if (typeof reply.networks === 'number') parts.push(`${reply.networks} network(s) stored`);
        // A kept password is worth saying; a blank one is worth saying loudly,
        // because a network saved with no password will silently fail to join
        // anything that is not actually open.
        if (typeof reply.keptPasswords === 'number' && reply.keptPasswords > 0) {
            parts.push(`${reply.keptPasswords} password(s) kept from device`);
        }
        if (typeof reply.blankPasswords === 'number') {
            parts.push(reply.blankPasswords > 0
                ? `${reply.blankPasswords} left blank (open network)`
                : 'none left blank');
        }
        if (reply.forgotten) parts.push('credentials wiped');
        return parts.length ? parts.join(', ') : JSON.stringify(reply);
    }

    async function send(command, timeout) {
        setBusy(true);
        try {
            const reply = await client.request(command, timeout);
            log(`< ${describeReply(reply)}`);
            noteDeviceSsids(reply);
            return reply;
        } catch (err) {
            log(await describeConnectError(err));
            return null;
        } finally {
            setBusy(false);
        }
    }

    provisionButton.addEventListener('click', async () => {
        const sendId = wantsUserId();
        const sendNets = wantsNetworks();
        if (!sendId && !sendNets) {
            log('Nothing selected. Tick "Write the user ID", "Write the WiFi networks", or both.');
            return;
        }

        const command = { cmd: 'provision' };
        const summary = [];

        if (sendId) {
            const userId = userIdInput.value.trim();
            if (!userId) {
                log('Enter a user ID first, or generate one — or untick "Write the user ID" to send only the networks.');
                return;
            }
            command.userId = userId;
            summary.push('the user ID');
        }

        let payload = [];
        if (sendNets) {
            payload = networks
                .map((n) => ({ ssid: n.ssid.trim(), psk: n.psk }))
                .filter((n) => n.ssid !== '');
            if (payload.length === 0) {
                // The device refuses "networks":[] on purpose, so never send it.
                log('The network list is empty. The device will not accept an empty list — add a network, ' +
                    'untick "Write the WiFi networks" to change only the user ID, or use "Erase WiFi + ID" ' +
                    'to clear the device.');
                return;
            }

            // Blank password + an SSID the device does not already store = an open
            // network. That is occasionally what someone means and usually not, so
            // it is confirmed rather than logged after the fact.
            const willBeBlank = payload.filter((n) => n.psk === '' && !deviceKnows(n.ssid));
            if (willBeBlank.length) {
                const names = willBeBlank.map((n) => `"${n.ssid}"`).join(', ');
                const known = deviceSsids
                    ? `The device does not currently store ${names}, so ${willBeBlank.length === 1 ? 'it' : 'they'} ` +
                    'will be saved with no password at all (an open network).'
                    : `We have not read this device's stored list yet, so ${names} may be saved with no password ` +
                    'at all. "Check status" first if you want to be sure.';
                if (!window.confirm(`${known}\n\nSend anyway?`)) {
                    log('Cancelled. Type the password, or run "Check status" to see which names the device already stores.');
                    return;
                }
            }

            command.networks = payload;
            summary.push(`${payload.length} network(s)`);
        }

        if (sendNets) {
            log(`Sending ${summary.join(' and ')}, networks in order: ${payload.map((n) => n.ssid).join(' -> ')}`);
        } else {
            log('Sending the user ID only. The stored networks and their passwords are left untouched.');
        }

        const reply = await send(command, TIMEOUT_PROVISION);
        if (reply && reply.ok !== false) {
            const notes = [];
            if (sendId) notes.push('User ID written.');
            if (sendNets) {
                notes.push('Networks written. Passwords are stored on the device and are never read back.');
            } else {
                notes.push('Networks were not part of this write and are unchanged.');
            }
            if (!sendId) notes.push('The user ID was not part of this write and is unchanged.');
            log(notes.join(' '));
            // A successful network write means the device now stores exactly what
            // we sent, so the per-row notes can be trusted again straight away.
            if (sendNets && !Array.isArray(reply.ssids)) {
                deviceSsids = payload.map((n) => n.ssid);
                refreshNetworkNotes();
            }
        }
    });

    // Raw diagnostics. Every field is shown, including ones we do not recognise,
    // because when provisioning fails the exact values are the point -- and on
    // the Watchy, whose serial emits nothing, this is the only way to see NVS
    // state at all. Labels are cosmetic; unknown keys fall through verbatim.
    const DIAG_LABELS = {
        nvsInit: 'NVS init',
        openRW: 'namespace open (read/write)',
        probeWrote: 'probe write',
        probeRead: 'probe read back',
        nvsUsed: 'NVS entries used',
        nvsFree: 'NVS entries free',
        nvsTotal: 'NVS entries total',
        heap: 'free heap (bytes)'
    };

    function describeDiag(reply) {
        if (!reply || typeof reply !== 'object') return String(reply);
        const lines = Object.keys(reply)
            .filter((key) => key !== 'ok')
            .map((key) => {
                const value = reply[key];
                const shown = typeof value === 'boolean' ? (value ? 'yes' : 'NO') : JSON.stringify(value);
                return `    ${DIAG_LABELS[key] || key}: ${shown}`;
            });
        if (reply.ok === false) lines.unshift('    reported failure');
        return lines.length ? `diagnostics\n${lines.join('\n')}` : 'diagnostics: empty reply';
    }

    if (diagButton) {
        diagButton.addEventListener('click', async () => {
            setBusy(true);
            try {
                const reply = await client.request({ cmd: 'diag' }, TIMEOUT_DEFAULT);
                log(`< ${describeDiag(reply)}`);
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
        if (reply && reply.ok !== false) {
            log('Device credentials wiped. A blank password field now means an open network, not a kept one.');
            deviceSsids = [];
            refreshNetworkNotes();
        }
    });
}
