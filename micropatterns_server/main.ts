import { serve } from "std/http/server.ts";
import * as s3 from "./s3.ts";

const PORT = 8000; // Default Deno Deploy port

async function handler(req: Request): Promise<Response> {
    const url = new URL(req.url);
    const path = url.pathname;
    const method = req.method;

    console.log(`[Server] ${method} ${path}`);

    // CORS Headers - Adjust origin as needed for security
    const corsHeaders = {
        "Access-Control-Allow-Origin": "*", // Allow requests from any origin (emulator)
        "Access-Control-Allow-Methods": "GET, PUT, OPTIONS",
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
                lastModified: new Date().toISOString(),
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