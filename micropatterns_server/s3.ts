import "dotenv"; // Load .env file
import { S3Client } from "s3_lite_client";
import { generatePublishId } from "./utils.ts"; // Added import

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

// --- Published Script Operations ---

/**
 * Defines the S3 key for a published script.
 * @param publishId The unique ID of the published script.
 * @returns The S3 key, e.g., published_scripts/publishId.json
 */
export function getPublishedScriptKey(publishId: string): string {
    // Basic sanitization for publishId - similar to scriptId
    const safePublishId = publishId.replace(/[^a-zA-Z0-9-_]/g, '');
    if (!safePublishId) throw new Error("Invalid publish ID provided.");
    return `published_scripts/${safePublishId}.json`;
}

/**
 * Saves a published script to S3.
 * @param publishId The unique ID for the published version.
 * @param scriptName The name of the script.
 * @param scriptContent The content of the script.
 * @returns Promise<boolean> True on success, false on failure.
 */
export async function savePublishedScript(publishId: string, scriptName: string, scriptContent: string): Promise<boolean> {
    const key = getPublishedScriptKey(publishId);
    try {
        const scriptData = {
            name: scriptName,
            content: scriptContent,
            publishedAt: new Date().toISOString(),
            publishID: publishId, // Store publishID for consistency, though key is derived from it
        };
        const dataString = JSON.stringify(scriptData, null, 2);
        await s3client.putObject(
            key,
            new TextEncoder().encode(dataString),
            { metadata: { "Content-Type": "application/json; charset=utf-8" } }
        );
        console.log(`[S3] Published script '${scriptName}' (publishId: '${publishId}', key: '${key}') saved successfully.`);
        return true;
    } catch (error) {
        console.error(`[S3] Error saving published script '${scriptName}' (publishId: '${publishId}', key: '${key}'):`, error.message);
        return false;
    }
}

/**
 * Retrieves a published script from S3.
 * @param publishId The unique ID of the published script.
 * @returns Promise<{ name: string, content: string, publishedAt: string, publishID: string } | null> The script data or null.
 */
export async function getPublishedScript(publishId: string): Promise<{ name: string, content: string, publishedAt: string, publishID: string } | null> {
    const key = getPublishedScriptKey(publishId);
    try {
        const response = await s3client.getObject(key);
        if (!response) return null;
        const content = await response.text();
        const scriptData = JSON.parse(content);
        // Ensure expected fields are present, though type is somewhat loose
        if (scriptData && typeof scriptData.name === 'string' && typeof scriptData.content === 'string') {
            return scriptData as { name: string, content: string, publishedAt: string, publishID: string };
        } else {
            console.error(`[S3] Published script '${publishId}' (key: '${key}') has invalid format.`);
            return null;
        }
    } catch (error) {
        if (error.message.includes("NoSuchKey") || error.message.includes("Object not found") || error.message.includes("404")) {
            console.log(`[S3] Published script '${publishId}' (key: '${key}') not found.`);
            return null;
        }
        console.error(`[S3] Error getting published script '${publishId}' (key: '${key}'):`, error.message, error);
        return null;
    }
}

/**
 * Deletes a published script from S3.
 * @param publishId The unique ID of the published script.
 * @returns Promise<boolean> True on success, false on failure.
 */
export async function deletePublishedScript(publishId: string): Promise<boolean> {
    const key = getPublishedScriptKey(publishId);
    try {
        // Assuming s3_lite_client uses removeObject for deletion.
        await s3client.removeObject(key);
        console.log(`[S3] Published script '${publishId}' (key: '${key}') deleted successfully.`);
        return true;
    } catch (error) {
        // It's often okay if the object doesn't exist when trying to delete.
        if (error.message.includes("NoSuchKey") || error.message.includes("Object not found") || error.message.includes("404")) {
            console.log(`[S3] Published script '${publishId}' (key: '${key}') not found during delete, considered successful.`);
            return true; 
        }
        console.error(`[S3] Error deleting published script '${publishId}' (key: '${key}'):`, error.message);
        return false;
    }
}

// --- User ID based S3 Key Generation ---
// Sanitize userID to prevent path traversal or invalid characters in S3 keys
function sanitizeUserIdForS3(userId: string): string {
    if (!userId) return "invalid_user"; // Fallback for empty userId
    // Allow alphanumeric, hyphen, underscore. Max length 50.
    return userId.replace(/[^a-zA-Z0-9-_]/g, '').substring(0, 50) || "sanitized_user_id";
}

const SCRIPTS_INDEX_KEY_FN = (userId: string) => `${sanitizeUserIdForS3(userId)}.json`;
const DEVICE_SCRIPTS_INDEX_KEY_FN = (userId: string) => `${sanitizeUserIdForS3(userId)}-device.json`;
const SCRIPT_PREFIX_FN = (userId: string) => `scripts/${sanitizeUserIdForS3(userId)}/`;


// --- Script Index Operations ---

export async function getScriptsIndex(userId: string): Promise<any[]> {
    const key = SCRIPTS_INDEX_KEY_FN(userId);
    try {
        const response = await s3client.getObject(key);
        if (!response) return []; // Index doesn't exist yet
        const content = await response.text();
        return JSON.parse(content || "[]");
    } catch (error) {
        if (error.message.includes("NoSuchKey") || error.message.includes("Object not found") || error.message.includes("404")) {
            console.log(`[S3] Scripts index for user '${userId}' (key: '${key}') not found, returning empty list.`);
            return [];
        }
        console.error(`[S3] Error getting scripts index for user '${userId}' (key: '${key}'):`, error.message);
        return [];
    }
}

export async function saveScriptsIndex(userId: string, indexData: any[]): Promise<boolean> {
    const key = SCRIPTS_INDEX_KEY_FN(userId);
    try {
        const dataString = JSON.stringify(indexData, null, 2);
        await s3client.putObject(
            key,
            new TextEncoder().encode(dataString),
            { metadata: { "Content-Type": "application/json; charset=utf-8" } }
        );
        console.log(`[S3] Scripts index for user '${userId}' (key: '${key}') saved successfully.`);
        return true;
    } catch (error) {
        console.error(`[S3] Error saving scripts index for user '${userId}' (key: '${key}'):`, error.message);
        return false;
    }
}

// --- Device-Specific Script Index Operations ---

export async function getDeviceScriptsIndex(userId: string): Promise<any[]> {
    const key = DEVICE_SCRIPTS_INDEX_KEY_FN(userId);
    try {
        const response = await s3client.getObject(key);
        if (!response) return [];
        const content = await response.text();
        return JSON.parse(content || "[]");
    } catch (error) {
        if (error.message.includes("NoSuchKey") || error.message.includes("Object not found") || error.message.includes("404")) {
            console.log(`[S3] Device scripts index for user '${userId}' (key: '${key}') not found, returning empty list.`);
            return [];
        }
        console.error(`[S3] Error getting device scripts index for user '${userId}' (key: '${key}'):`, error.message);
        return [];
    }
}

export async function saveDeviceScriptsIndex(userId: string, indexData: any[]): Promise<boolean> {
    const key = DEVICE_SCRIPTS_INDEX_KEY_FN(userId);
    try {
        const dataString = JSON.stringify(indexData, null, 2);
        await s3client.putObject(
            key,
            new TextEncoder().encode(dataString),
            { metadata: { "Content-Type": "application/json; charset=utf-8" } }
        );
        console.log(`[S3] Device scripts index for user '${userId}' (key: '${key}') saved successfully.`);
        return true;
    } catch (error) {
        console.error(`[S3] Error saving device scripts index for user '${userId}' (key: '${key}'):`, error.message);
        return false;
    }
}


// --- Individual Script Operations ---

function getScriptKey(userId: string, scriptId: string): string {
    const safeUserId = sanitizeUserIdForS3(userId);
    // Basic sanitization for scriptId - remove potential path traversal chars, ensure valid filename
    const safeScriptId = scriptId.replace(/[^a-zA-Z0-9-_]/g, '');
    if (!safeScriptId) throw new Error("Invalid script ID provided.");
    return `${SCRIPT_PREFIX_FN(safeUserId)}${safeScriptId}.json`;
}

export async function getScript(userId: string, scriptId: string): Promise<any | null> {
    // Note: This function returns `any`. If the script data in S3 contains a `publishID` field,
    // it will be part of the returned object. The ScriptData type is implicitly:
    // { id: string, name: string, content: string, lastModified: string, publishID?: string }
    const key = getScriptKey(userId, scriptId);
    try {
        const response = await s3client.getObject(key);
        if (!response) return null;
        const content = await response.text();
        return JSON.parse(content); // This will include publishID if present in the S3 object
    } catch (error) {
         if (error.message.includes("NoSuchKey") || error.message.includes("Object not found") || error.message.includes("404")) {
            console.log(`[S3] Script '${scriptId}' for user '${userId}' (key: '${key}') not found.`);
            return null;
        }
        console.error(`[S3] Error getting script '${scriptId}' for user '${userId}' (key: '${key}'):`, error.message);
        return null;
    }
}

export async function saveScript(userId: string, scriptId: string, scriptData: any): Promise<boolean> {
    const key = getScriptKey(userId, scriptId);
    let mainScriptSaved = false;
    try {
        // Ensure lastModified is updated before saving
        scriptData.lastModified = new Date().toISOString(); 
        const dataString = JSON.stringify(scriptData, null, 2);
        await s3client.putObject(
            key,
            new TextEncoder().encode(dataString),
            { metadata: { "Content-Type": "application/json; charset=utf-8" } }
        );
        console.log(`[S3] Script '${scriptId}' for user '${userId}' (key: '${key}') saved successfully.`);
        mainScriptSaved = true;
    } catch (error) {
        console.error(`[S3] Error saving script '${scriptId}' for user '${userId}' (key: '${key}'):`, error.message);
        return false; // Primary operation failed
    }

    // After successfully saving the main script, check for publishID and save/update the published version.
    if (mainScriptSaved && scriptData && typeof scriptData.publishID === 'string' && scriptData.publishID.trim() !== '') {
        console.log(`[S3] Script '${scriptId}' has a publishID ('${scriptData.publishID}'). Attempting to save/update published version.`);
        // Use the name and content from the scriptData being saved
        const publishedSaved = await savePublishedScript(scriptData.publishID, scriptData.name, scriptData.content);
        if (publishedSaved) {
            console.log(`[S3] Published version for script '${scriptId}' (publishID: '${scriptData.publishID}') saved/updated successfully.`);
        } else {
            // Log error, but don't mark saveScript as failed if only this part fails.
            // The main script was saved. This part is secondary; an error here means the published version might be stale.
            console.error(`[S3] Failed to save/update published version for script '${scriptId}' (publishID: '${scriptData.publishID}'). Main script is saved, but published version may be stale.`);
        }
    } else if (mainScriptSaved) {
        // This case means either no publishID was provided, or it was empty.
        // If a script was previously published and then its publishID is removed (e.g. set to "" or null),
        // we might want to delete the old published version. This is not explicitly handled here yet.
        // For now, we just log that no action is taken for the published version.
        console.log(`[S3] Script '${scriptId}' does not have a (valid) publishID. No action taken for published version.`);
    }
    
    return mainScriptSaved; // Success of saveScript depends on the main script save operation
}

// Utility to generate a simple ID from a name
export function generateIdFromName(name: string): string {
    if (!name) return `script-${Date.now()}`; // Fallback if name is empty
    return name.toLowerCase()
               .replace(/\s+/g, '-') // Replace spaces with hyphens
               .replace(/[^a-z0-9-]/g, '') // Remove invalid characters
               .substring(0, 50); // Limit length
}