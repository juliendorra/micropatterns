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

            const scriptData = {
                id: scriptId, // The scriptId from the path is the canonical one
                name: requestBody.name,
                content: requestBody.content,
                lastModified: new Date().toISOString(), // Will be updated by s3.saveScript again, but good to have here
                publishID: requestBody.publishID // Pass through publishID if provided
            };

            const scriptSaveSuccess = await s3.saveScript(userId, scriptId, scriptData);
            if (!scriptSaveSuccess) {
                return new Response(JSON.stringify({ error: "Failed to save script file" }), {
                    status: 500,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            let index = await s3.getScriptsIndex(userId);
            const existingIndexEntry = index.findIndex(item => item.id === scriptId);
            const indexEntry = { id: scriptId, name: scriptData.name, lastModified: scriptData.lastModified };

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

            return new Response(JSON.stringify({ success: true, script: scriptData }), {
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

            // If not already published, generate a new publishID.
            // If already published, this effectively re-publishes/updates content with the existing publishID.
            if (!scriptData.publishID) {
                scriptData.publishID = generatePublishId();
            }
            // s3.saveScript will handle saving the main script and the published version
            const saveSuccess = await s3.saveScript(userId, scriptId, scriptData);

            if (saveSuccess) {
                return new Response(JSON.stringify({ success: true, publishID: scriptData.publishID }), {
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

            const oldPublishID = scriptData.publishID;
            scriptData.publishID = null; // Or delete scriptData.publishID;

            const saveSuccess = await s3.saveScript(userId, scriptId, scriptData);
            if (!saveSuccess) {
                return new Response(JSON.stringify({ error: "Failed to update script metadata during unpublish" }), {
                    status: 500,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            // If metadata saved successfully, delete the published script from its dedicated S3 location
            const deleteSuccess = await s3.deletePublishedScript(oldPublishID);
            if (!deleteSuccess) {
                // Log this error, but main unpublish operation (removing publishID from script) succeeded
                console.error(`[Server] Script ${scriptId} for user ${userId} unpublished, but failed to delete published content for ${oldPublishID}.`);
            }

            return new Response(JSON.stringify({ success: true }), {
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