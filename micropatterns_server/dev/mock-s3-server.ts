import { existsSync } from "https://deno.land/std/fs/mod.ts";
import { ensureDirSync } from "https://deno.land/std/fs/ensure_dir.ts";
import { join } from "https://deno.land/std/path/mod.ts";

const LOCAL_STORAGE_DIR = "./local-s3-storage";

ensureDirSync(LOCAL_STORAGE_DIR);

async function handleRequest(req: Request): Promise<Response> {
    const url = new URL(req.url);
    // Key includes bucket name, e.g., /my-bucket/scripts.json or /my-bucket/scripts/my-script.json
    const keyWithBucket = url.pathname.substring(1);

    // Extract bucket and actual object key
    const firstSlashIndex = keyWithBucket.indexOf('/');
    if (firstSlashIndex === -1) {
        console.error("[MOCK S3] Invalid path, missing bucket:", keyWithBucket);
        return new Response("Invalid path format (missing bucket)", { status: 400 });
    }
    const bucket = keyWithBucket.substring(0, firstSlashIndex);
    const objectKey = keyWithBucket.substring(firstSlashIndex + 1);

    if (!bucket || !objectKey) {
        console.error("[MOCK S3] Invalid bucket or object key:", { bucket, objectKey });
        return new Response("Invalid bucket or object key", { status: 400 });
    }

    console.log(`[MOCK S3] Request: ${req.method} Bucket: ${bucket} Key: ${objectKey}`);

    // We ignore the bucket name for local storage path, just use the object key
    const storagePath = join(LOCAL_STORAGE_DIR, objectKey);

    switch (req.method) {
        case "PUT":
            {
                const body = await req.arrayBuffer();
                await putObject(objectKey, new Uint8Array(body)); // Use objectKey for storage path logic
                console.log(`[MOCK S3] PUT successful for key: ${objectKey}`);
                // S3 PUT usually returns ETag header, but empty body is fine for mock
                return new Response(null, { status: 200 });
            }

        case "GET":
            try {
                const data = await getObject(objectKey); // Use objectKey for storage path logic
                console.log(`[MOCK S3] GET successful for key: ${objectKey}`);
                return new Response(data, { status: 200 });
            } catch (error) {
                console.warn(`[MOCK S3] GET failed for key: ${objectKey} - ${error.message}`);
                return new Response("Object not found", { status: 404 });
            }

        case "HEAD":
            {
                const exists = await objectExists(objectKey); // Use objectKey for storage path logic
                console.log(`[MOCK S3] HEAD check for key: ${objectKey} - Exists: ${exists}`);
                return new Response(null, { status: exists ? 200 : 404 });
            }
        default:
            return new Response("Method not allowed", { status: 405 });
    }
}

// key is the object key (e.g., scripts.json or scripts/my-script.json)
async function putObject(key: string, data: Uint8Array) {
    const filePath = join(LOCAL_STORAGE_DIR, key);
    const dirPath = join(LOCAL_STORAGE_DIR, key.split("/").slice(0, -1).join("/"));
    ensureDirSync(dirPath); // Ensure the directory exists
    await Deno.writeFile(filePath, data);
    console.log(`[MOCK S3 Storage] Wrote ${data.length} bytes to ${filePath}`);
}

// key is the object key
async function getObject(key: string): Promise<Uint8Array> {
    const filePath = join(LOCAL_STORAGE_DIR, key);
    if (!existsSync(filePath)) {
        throw new Error(`Object not found at ${filePath}`);
    }
    console.log(`[MOCK S3 Storage] Reading from ${filePath}`);
    return await Deno.readFile(filePath);
}

// key is the object key
async function objectExists(key: string): Promise<boolean> {
    const filePath = join(LOCAL_STORAGE_DIR, key);
    const exists = existsSync(filePath);
    console.log(`[MOCK S3 Storage] Checking existence of ${filePath}: ${exists}`);
    return exists;
}

// Default port for mock server
const MOCK_S3_PORT = 443;
console.log(`Mock S3 server running on http://localhost:${MOCK_S3_PORT}`);
console.log(`Local storage directory: ${LOCAL_STORAGE_DIR}`);

Deno.serve({ port: MOCK_S3_PORT }, handleRequest);