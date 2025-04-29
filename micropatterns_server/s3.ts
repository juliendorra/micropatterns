import "dotenv"; // Load .env file
import { S3Client } from "s3_lite_client";

const S3_ENDPOINT = Deno.env.get("S3_ENDPOINT") || "";
const S3_REGION = Deno.env.get("S3_REGION") || "auto"; // Default region for R2 etc.
const S3_BUCKET = Deno.env.get("S3_BUCKET") || "micropatterns-scripts"; // Default bucket name
const S3_ACCESS_KEY_ID = Deno.env.get("S3_ACCESS_KEY_ID") || "";
const SECRET_ACCESS_KEY = Deno.env.get("SECRET_ACCESS_KEY") || "";
const S3_IS_LOCAL = Deno.env.get("S3_IS_LOCAL") === "true";

let useSSL = !S3_IS_LOCAL;
let port = useSSL ? 443 : 80; // Default ports

// Allow overriding port for local dev if endpoint includes it
let endPoint = S3_ENDPOINT;
if (S3_IS_LOCAL && S3_ENDPOINT.includes(':')) {
    const parts = S3_ENDPOINT.split(':');
    endPoint = parts[0];
    port = parseInt(parts[1], 10) || 80; // Use specified port or default
    console.log(`[S3 Client] Using local endpoint ${endPoint} on port ${port}`);
} else if (S3_IS_LOCAL) {
    // Default local mock S3 port if not specified in endpoint
    port = 443; // mock-s3-server runs on 443 by default
    console.log(`[S3 Client] Using local endpoint ${endPoint} on default port ${port}`);
}


if (!S3_ENDPOINT || !S3_BUCKET || (!S3_IS_LOCAL && (!S3_ACCESS_KEY_ID || !SECRET_ACCESS_KEY))) {
    console.warn("------------------------------------------------------");
    console.warn("S3 Environment Variables Missing!");
    console.warn("S3_ENDPOINT, S3_BUCKET are required.");
    console.warn("S3_ACCESS_KEY_ID, SECRET_ACCESS_KEY are required unless S3_IS_LOCAL=true.");
    console.warn("Using default/empty values, S3 operations might fail.");
    console.warn("------------------------------------------------------");
}


const s3client = new S3Client({
  endPoint: endPoint,
  port: port,
  useSSL: useSSL,
  region: S3_REGION,
  accessKey: S3_ACCESS_KEY_ID,
  secretKey: SECRET_ACCESS_KEY,
  bucket: S3_BUCKET,
  pathStyle: true, // Important for MinIO, R2, and potentially mock server
});

const SCRIPTS_INDEX_KEY = "scripts.json";
const SCRIPT_PREFIX = "scripts/"; // Folder for individual scripts

// --- Script Index Operations ---

export async function getScriptsIndex(): Promise<any[]> {
    try {
        const response = await s3client.getObject(SCRIPTS_INDEX_KEY);
        if (!response) return []; // Index doesn't exist yet
        const content = await response.text();
        return JSON.parse(content || "[]");
    } catch (error) {
        // If index doesn't exist (404), return empty array, otherwise log error
        if (error.message.includes("NoSuchKey") || error.message.includes("Object not found")) {
            console.log("[S3] Scripts index not found, returning empty list.");
            return [];
        }
        console.error("[S3] Error getting scripts index:", error.message);
        return []; // Return empty on other errors too
    }
}

export async function saveScriptsIndex(indexData: any[]): Promise<boolean> {
    try {
        const dataString = JSON.stringify(indexData, null, 2); // Pretty print for readability
        await s3client.putObject(
            SCRIPTS_INDEX_KEY,
            new TextEncoder().encode(dataString),
            { metadata: { "Content-Type": "application/json; charset=utf-8" } }
        );
        console.log("[S3] Scripts index saved successfully.");
        return true;
    } catch (error) {
        console.error("[S3] Error saving scripts index:", error.message);
        return false;
    }
}

// --- Individual Script Operations ---

function getScriptKey(scriptId: string): string {
    // Basic sanitization - remove potential path traversal chars, ensure valid filename
    const safeId = scriptId.replace(/[^a-zA-Z0-9-_]/g, '');
    if (!safeId) throw new Error("Invalid script ID provided.");
    return `${SCRIPT_PREFIX}${safeId}.json`;
}

export async function getScript(scriptId: string): Promise<any | null> {
    const key = getScriptKey(scriptId);
    try {
        const response = await s3client.getObject(key);
        if (!response) return null;
        const content = await response.text();
        return JSON.parse(content);
    } catch (error) {
         if (error.message.includes("NoSuchKey") || error.message.includes("Object not found")) {
            console.log(`[S3] Script '${scriptId}' not found.`);
            return null;
        }
        console.error(`[S3] Error getting script '${scriptId}':`, error.message);
        return null;
    }
}

export async function saveScript(scriptId: string, scriptData: any): Promise<boolean> {
    const key = getScriptKey(scriptId);
    try {
        const dataString = JSON.stringify(scriptData, null, 2);
        await s3client.putObject(
            key,
            new TextEncoder().encode(dataString),
            { metadata: { "Content-Type": "application/json; charset=utf-8" } }
        );
        console.log(`[S3] Script '${scriptId}' saved successfully.`);
        return true;
    } catch (error) {
        console.error(`[S3] Error saving script '${scriptId}':`, error.message);
        return false;
    }
}

// Utility to generate a simple ID from a name
export function generateIdFromName(name: string): string {
    if (!name) return `script-${Date.now()}`; // Fallback if name is empty
    return name.toLowerCase()
               .replace(/\s+/g, '-') // Replace spaces with hyphens
               .replace(/[^a-z0-9-]/g, '') // Remove invalid characters
               .substring(0, 50); // Limit length
}