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

        // GET /api/scripts - Get full script index
        if (path === "/api/scripts" && method === "GET") {
            const index = await s3.getScriptsIndex();
            return new Response(JSON.stringify(index), {
                status: 200,
                headers: { ...corsHeaders, "Content-Type": "application/json" },
            });
        }

        // GET /api/device/scripts - Returns the list of scripts selected for device sync
        if (path === "/api/device/scripts" && method === "GET") {
            const deviceIndex = await s3.getDeviceScriptsIndex();
            return new Response(JSON.stringify(deviceIndex), {
                status: 200,
                headers: { ...corsHeaders, "Content-Type": "application/json" },
            });
        }

        // PUT /api/device/scripts - Updates the list of scripts for device sync
        if (path === "/api/device/scripts" && method === "PUT") {
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

            // Fetch all scripts to ensure we only add existing ones
            const allScripts = await s3.getScriptsIndex();
            const deviceScripts = allScripts.filter(script => selectedIds.includes(script.id));
            
            // The deviceScripts array now contains objects like {id, name} for selected scripts

            const saveSuccess = await s3.saveDeviceScriptsIndex(deviceScripts);
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

        // Matches /api/scripts/:scriptId
        const scriptMatch = path.match(/^\/api\/scripts\/([a-zA-Z0-9-_]+)$/);
        if (scriptMatch && method === "GET") {
            // Get single script
            const scriptId = scriptMatch[1];
            const scriptData = await s3.getScript(scriptId);
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

        if (scriptMatch && method === "PUT") {
            // Save/Update single script
            const scriptId = scriptMatch[1];
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

            // Prepare script data for S3
            const scriptData = {
                id: scriptId,
                name: requestBody.name,
                content: requestBody.content,
                lastModified: new Date().toISOString(),
            };

            // Save the individual script file
            const scriptSaveSuccess = await s3.saveScript(scriptId, scriptData);
            if (!scriptSaveSuccess) {
                return new Response(JSON.stringify({ error: "Failed to save script file" }), {
                    status: 500,
                    headers: { ...corsHeaders, "Content-Type": "application/json" },
                });
            }

            // Update the index
            let index = await s3.getScriptsIndex();
            const existingIndex = index.findIndex(item => item.id === scriptId);
            const indexEntry = { id: scriptId, name: scriptData.name };

            if (existingIndex !== -1) {
                index[existingIndex] = indexEntry; // Update existing entry
            } else {
                index.push(indexEntry); // Add new entry
            }
            // Sort index alphabetically by name for consistent listing
            index.sort((a, b) => a.name.localeCompare(b.name));

            const indexSaveSuccess = await s3.saveScriptsIndex(index);
            if (!indexSaveSuccess) {
                 console.error(`[Server] Script ${scriptId} saved, but failed to update index.`);
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