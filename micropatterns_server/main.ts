import { serve } from "std/http/server.ts";
import * as s3 from "./s3.ts";
import { generatePublishId } from "./utils.ts"; // Added for publish endpoint

const PORT = 8000; // Default Deno Deploy port

async function handler(req: Request): Promise<Response> {
    const url = new URL(req.url);
    const path = url.pathname;
    const method = req.method;

    console.log(`[Server] ${method} ${path}`);

    // CORS Headers - Adjust origin as needed for security
    const corsHeaders = {
        "Access-Control-Allow-Origin": "*", // Allow requests from any origin (emulator)
        "Access-Control-Allow-Methods": "GET, PUT, POST, OPTIONS", // Added POST
        "Access-Control-Allow-Headers": "Content-Type",
    };

    // Handle CORS preflight requests
    if (method === "OPTIONS") {
        return new Response(null, { status: 204, headers: corsHeaders });
    }

    try {
        // --- API Routes ---
        // User ID regex part: [a-zA-Z0-9-_]{10} (assuming 10 char nanoid, can be more flexible)
        // Script ID regex part: [a-zA-Z0-9-_]+

        // GET /api/scripts/:userID - Get script index for a user
        const userScriptsIndexMatch = path.match(/^\/api\/scripts\/([a-zA-Z0-9-_]{1,50})$/);
        if (userScriptsIndexMatch && method === "GET") {
            const userId = userScriptsIndexMatch[1];
            const index = await s3.getScriptsIndex(userId);
            return new Response(JSON.stringify(index), {
                status: 200,
                headers: { ...corsHeaders, "Content-Type": "application/json" },
            });
        }

        // GET /api/device/scripts/:userID - Get device script list for a user
        const userDeviceScriptsGetMatch = path.match(/^\/api\/device\/scripts\/([a-zA-Z0-9-_]{1,50})$/);
        if (userDeviceScriptsGetMatch && method === "GET") {
            const userId = userDeviceScriptsGetMatch[1];
            const deviceIndex = await s3.getDeviceScriptsIndex(userId);
            return new Response(JSON.stringify(deviceIndex), {
                status: 200,
                headers: { ...corsHeaders, "Content-Type": "application/json" },
            });
        }

        // PUT /api/device/scripts/:userID - Update device script list for a user
        const userDeviceScriptsPutMatch = path.match(/^\/api\/device\/scripts\/([a-zA-Z0-9-_]{1,50})$/);
        if (userDeviceScriptsPutMatch && method === "PUT") {
            const userId = userDeviceScriptsPutMatch[1];
            let requestBody;
            try {
                requestBody = await req.json();
            } catch (e) {
                return new Response(JSON.stringify({ error: "Invalid JSON body" }), {
                    status: 400,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            if (!requestBody || !Array.isArray(requestBody.selectedIds)) {
                return new Response(JSON.stringify({ error: "Missing 'selectedIds' array in request body" }), {
                    status: 400,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }
            const selectedIds: string[] = requestBody.selectedIds;

            const allScripts = await s3.getScriptsIndex(userId);
            const deviceScripts = allScripts.filter(script => selectedIds.includes(script.id));
            
            const saveSuccess = await s3.saveDeviceScriptsIndex(userId, deviceScripts);
            if (saveSuccess) {
                return new Response(JSON.stringify({ success: true, count: deviceScripts.length }), {
                    status: 200,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            } else {
                return new Response(JSON.stringify({ error: "Failed to save device script selection" }), {
                    status: 500,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }
        }

        // Matches /api/scripts/:userID/:scriptID
        const scriptDetailMatch = path.match(/^\/api\/scripts\/([a-zA-Z0-9-_]{1,50})\/([a-zA-Z0-9-_]+)$/);
        if (scriptDetailMatch && method === "GET") {
            const userId = scriptDetailMatch[1];
            const scriptId = scriptDetailMatch[2];
            const scriptData = await s3.getScript(userId, scriptId);
            if (scriptData) {
                return new Response(JSON.stringify(scriptData), {
                    status: 200,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            } else {
                return new Response(JSON.stringify({ error: "Script not found" }), {
                    status: 404,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }
        }

        if (scriptDetailMatch && method === "PUT") {
            const userId = scriptDetailMatch[1];
            const scriptId = scriptDetailMatch[2];
            let requestBody;
            try {
                requestBody = await req.json();
            } catch (e) {
                 return new Response(JSON.stringify({ error: "Invalid JSON body" }), {
                    status: 400,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            if (!requestBody || typeof requestBody.name !== 'string' || typeof requestBody.content !== 'string') {
                return new Response(JSON.stringify({ error: "Missing 'name' or 'content' in request body" }), {
                    status: 400,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            const existingScript = await s3.getScript(userId, scriptId);

            const updatedScriptData = {
                ...(existingScript || {}), // Start with existing data or empty object if not found
                id: scriptId,
                name: requestBody.name,
                content: requestBody.content,
                // lastModified will be set by s3.saveScript
                // publishID and isPublished are preserved from existingScript unless client overrides for some reason
                // However, general saves should not modify publish status - that's for publish/unpublish endpoints.
                // So, we prioritize existing values for publishID and isPublished.
                publishID: existingScript?.publishID || null,
                isPublished: existingScript?.isPublished || false,
            };
            // If requestBody *does* contain publishID or isPublished, it's likely from an older client or misuse.
            // For robustness, let's log if they are unexpectedly provided in a general save.
            if (requestBody.publishID !== undefined && requestBody.publishID !== updatedScriptData.publishID) {
                console.warn(`[Server PUT /script] requestBody contained publishID ${requestBody.publishID}, but preserving existing ${updatedScriptData.publishID}`);
            }
            if (typeof requestBody.isPublished === 'boolean' && requestBody.isPublished !== updatedScriptData.isPublished) {
                 console.warn(`[Server PUT /script] requestBody contained isPublished ${requestBody.isPublished}, but preserving existing ${updatedScriptData.isPublished}`);
            }


            const scriptSaveSuccess = await s3.saveScript(userId, scriptId, updatedScriptData);
            if (!scriptSaveSuccess) {
                return new Response(JSON.stringify({ error: "Failed to save script file" }), {
                    status: 500,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            const finalScriptData = await s3.getScript(userId, scriptId); // Get definitive saved data

            let index = await s3.getScriptsIndex(userId);
            const existingIndexEntry = index.findIndex(item => item.id === scriptId);
            const indexEntry = {
                id: scriptId,
                name: finalScriptData?.name || scriptId,
                lastModified: finalScriptData?.lastModified || new Date().toISOString()
            };

            if (existingIndexEntry !== -1) {
                index[existingIndexEntry] = indexEntry;
            } else {
                index.push(indexEntry);
            }
            index.sort((a, b) => a.name.localeCompare(b.name));

            const indexSaveSuccess = await s3.saveScriptsIndex(userId, index);
            if (!indexSaveSuccess) {
                 console.error(`[Server] Script ${scriptId} for user ${userId} saved, but failed to update index.`);
            }

            return new Response(JSON.stringify({ success: true, script: finalScriptData }), { // Return final data
                status: 200,
                headers: { ...corsHeaders, "Content-Type": "application/json" },
            });
        }

        // POST /api/scripts/:userID/:scriptID/publish - Publish a script
        const publishMatch = path.match(/^\/api\/scripts\/([a-zA-Z0-9-_]{1,50})\/([a-zA-Z0-9-_]+)\/publish$/);
        if (publishMatch && method === "POST") {
            const userId = publishMatch[1];
            const scriptId = publishMatch[2];

            const scriptData = await s3.getScript(userId, scriptId);
            if (!scriptData) {
                return new Response(JSON.stringify({ error: "Script not found to publish" }), {
                    status: 404,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            // Ensure scriptData is an object before trying to access/set properties
            if (typeof scriptData !== 'object' || scriptData === null) {
                 return new Response(JSON.stringify({ error: "Script data is invalid" }), {
                    status: 500, headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            // If scriptData.publishID is null, undefined, or an empty string, generate a new one.
            if (!scriptData.publishID || (typeof scriptData.publishID === 'string' && scriptData.publishID.trim() === '')) {
                scriptData.publishID = generatePublishId();
                console.log(`[Server /publish] Generated new publishID: ${scriptData.publishID} for script ${scriptId}`);
            }
            scriptData.isPublished = true; // Mark as published

            const saveSuccess = await s3.saveScript(userId, scriptId, scriptData);

            if (saveSuccess) {
                // Fetch the script again to get the definitive saved state
                const finalScriptData = await s3.getScript(userId, scriptId);
                return new Response(JSON.stringify({
                    success: true,
                    publishID: finalScriptData?.publishID,
                    isPublished: finalScriptData?.isPublished,
                    script: finalScriptData
                }), {
                    status: 200,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            } else {
                return new Response(JSON.stringify({ error: "Failed to publish script" }), {
                    status: 500,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }
        }

        // POST /api/scripts/:userID/:scriptID/unpublish - Unpublish a script
        const unpublishMatch = path.match(/^\/api\/scripts\/([a-zA-Z0-9-_]{1,50})\/([a-zA-Z0-9-_]+)\/unpublish$/);
        if (unpublishMatch && method === "POST") {
            const userId = unpublishMatch[1];
            const scriptId = unpublishMatch[2];

            const scriptData = await s3.getScript(userId, scriptId);
            if (!scriptData) {
                return new Response(JSON.stringify({ error: "Script not found to unpublish" }), {
                    status: 404,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            if (!scriptData.publishID) {
                return new Response(JSON.stringify({ error: "Script is not published" }), {
                    status: 400, // Bad request, as it's not published
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            // Ensure scriptData is an object
             if (typeof scriptData !== 'object' || scriptData === null) {
                 return new Response(JSON.stringify({ error: "Script data is invalid for unpublish" }), {
                    status: 500, headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            const publishIdToUnpublish = scriptData.publishID;
            scriptData.isPublished = false;

            const metadataSaveSuccess = await s3.saveScript(userId, scriptId, scriptData);
            if (!metadataSaveSuccess) {
                return new Response(JSON.stringify({ error: "Failed to update script metadata for unpublish" }), {
                    status: 500,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            if (publishIdToUnpublish) {
                const publicFileDeleteSuccess = await s3.deletePublishedScript(publishIdToUnpublish);
                if (publicFileDeleteSuccess) {
                    console.log(`[Server /unpublish] Deleted published content for ${publishIdToUnpublish}.`);
                } else {
                    console.error(`[Server /unpublish] FAILED to delete published content for ${publishIdToUnpublish}. Metadata is unpublished.`);
                }
            } else {
                 console.warn(`[Server /unpublish] No publishID found on script ${scriptId} to delete from public store.`);
            }

            const finalScriptData = await s3.getScript(userId, scriptId); // Get definitive state
            return new Response(JSON.stringify({
                success: true,
                publishID: finalScriptData?.publishID,
                isPublished: finalScriptData?.isPublished,
                script: finalScriptData
            }), {
                status: 200,
                headers: { ...corsHeaders, "Content-Type": "application/json" },
            });
        }

        // GET /api/view/:publishID - View a published script
        const viewMatch = path.match(/^\/api\/view\/([a-zA-Z0-9-_]{21})$/); // Assuming 21 char nanoid for publishID
        if (viewMatch && method === "GET") {
            const publishId = viewMatch[1];
            const publishedScript = await s3.getPublishedScript(publishId);

            if (publishedScript) {
                return new Response(JSON.stringify(publishedScript), {
                    status: 200,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            } else {
                return new Response(JSON.stringify({ error: "Published script not found" }), {
                    status: 404,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }
        }

        // --- Default Route ---
        return new Response(JSON.stringify({ message: "Micropatterns API Server" }), {
            status: 200,
            headers: { ...corsHeaders, "Content-Type": "application/json" },
        });

    } catch (error) {
        console.error("[Server] Error handling request:", error);
        return new Response(JSON.stringify({ error: "Internal Server Error" }), {
            status: 500,
            headers: { ...corsHeaders, "Content-Type": "application/json" },
        });
    }
}

console.log(`[Server] MicroPatterns API server running on http://localhost:${PORT}`);
serve(handler, { port: PORT });