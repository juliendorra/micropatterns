# MicroPatterns Server

This Deno server provides a simple API for storing and retrieving MicroPatterns scripts. It uses an S3-compatible object storage (like AWS S3, Cloudflare R2, or a local mock server) to persist the scripts.

## Features

*   List all available scripts.
*   Retrieve the content of a specific script.
*   Save new scripts or update existing ones.
*   Maintains an index file (`scripts.json`) in the S3 bucket for quick listing by clients (like the emulator or ESP32 devices).

## Technology

*   **Runtime:** [Deno](https://deno.land/)
*   **Storage:** S3-compatible object storage (via `s3_lite_client`)

## Setup

### 1. Environment Variables

The server requires environment variables for configuration, primarily for connecting to the S3 bucket. Create a `.env` file in the `micropatterns_server` directory with the following variables:

```dotenv
# S3 Configuration
S3_ENDPOINT=your_s3_endpoint_url # e.g., s3.us-west-2.amazonaws.com or accountid.r2.cloudflarestorage.com
S3_REGION=your_s3_region # e.g., us-west-2 or auto
S3_BUCKET=your_bucket_name # e.g., micropatterns-scripts
S3_ACCESS_KEY_ID=your_access_key_id
SECRET_ACCESS_KEY=your_secret_access_key

# Set to true if using the local mock S3 server
S3_IS_LOCAL=false
```

**Notes:**

*   `S3_ACCESS_KEY_ID` and `SECRET_ACCESS_KEY` are only required if `S3_IS_LOCAL` is `false`.
*   For Cloudflare R2, `S3_REGION` should typically be set to `auto`.
*   If `S3_IS_LOCAL=true`, the server will attempt to connect to `http://localhost:443` by default (the port used by `mock-s3-server.ts`). You can override this by including the port in `S3_ENDPOINT`, e.g., `S3_ENDPOINT=localhost:9000`.

### 2. Running Locally (Development)

**Option A: Using Mock S3 Server (Recommended for local dev)**

1.  **Start the Mock S3 Server:**
    ```bash
    deno task mock-s3
    ```
    This will create a `local-s3-storage` directory to simulate the bucket. Ensure `S3_IS_LOCAL=true` in your `.env` file. The mock server runs on port 443 by default.

2.  **Start the API Server:**
    In a separate terminal:
    ```bash
    deno task dev
    ```
    This starts the main API server with hot-reloading, watching for file changes. It will connect to the mock S3 server running locally.

**Option B: Using a Real S3 Bucket**

1.  Ensure your `.env` file is configured with your actual S3 credentials and `S3_IS_LOCAL=false`.
2.  **Start the API Server:**
    ```bash
    deno task dev
    ```

### 3. Running in Production (e.g., Deno Deploy)

1.  **Set Environment Variables:** Configure the necessary S3 environment variables in your Deno Deploy project settings. Ensure `S3_IS_LOCAL` is set to `false`.
2.  **Deploy:** Link your GitHub repository to Deno Deploy and choose `main.ts` as the entry point. Deno Deploy will automatically use the `deno task start` command equivalent.

## API Endpoints

The server exposes the following endpoints:

*   **`GET /api/scripts`**
    *   **Description:** Retrieves the list of available scripts from the `scripts.json` index file in the S3 bucket.
    *   **Response:** `200 OK` with a JSON array:
        ```json
        [
          { "id": "script-id-1", "name": "Cool Pattern", "lastModified": "2023-10-27T10:00:00.000Z" },
          { "id": "script-id-2", "name": "Another One", "lastModified": "2023-10-28T11:00:00.000Z" }
        ]
        ```

*   **`GET /api/device/scripts`**
    *   **Description:** Retrieves the list of scripts that have been selected for synchronization with devices. This list is a subset of all available scripts.
    *   **Response:** `200 OK` with a JSON array:
        ```json
        [
          { "id": "script-id-1", "name": "Cool Pattern", "lastModified": "2023-10-27T10:00:00.000Z" },
          { "id": "script-id-3", "name": "Device Favorite", "lastModified": "2023-10-29T12:00:00.000Z" }
        ]
        ```
    *   If the device-specific index doesn't exist or is empty, returns an empty array `[]`.

*   **`PUT /api/device/scripts`**
    *   **Description:** Updates the list of scripts selected for device synchronization. This overwrites the existing `scripts-device.json` index file in the S3 bucket.
    *   **Request Body:** JSON object:
        ```json
        {
          "selectedIds": ["script-id-1", "script-id-3"]
        }
        ```
    *   **Response:**
        *   `200 OK` on success:
            ```json
            {
              "success": true,
              "count": 2 // Number of scripts in the updated device list
            }
            ```
        *   `400 Bad Request` if `selectedIds` is missing or not an array.
        *   `500 Internal Server Error` if saving to S3 fails.

*   **`GET /api/scripts/:id`**
    *   **Description:** Retrieves the full data for a specific script.
    *   **Parameters:** `:id` - The unique ID of the script (usually derived from the name).
    *   **Response:**
        *   `200 OK` with the script data:
            ```json
            {
              "id": "script-id-1",
              "name": "Cool Pattern",
              "content": "# MicroPatterns Script Content...\nCOLOR NAME=BLACK\n...",
              "lastModified": "2023-10-27T10:00:00.000Z"
            }
            ```
        *   `404 Not Found` if the script ID doesn't exist.

*   **`PUT /api/scripts/:id`**
    *   **Description:** Saves a new script or updates an existing one. This operation saves the individual script file (`scripts/<id>.json`) and then rebuilds and overwrites the main index file (`scripts.json`).
    *   **Parameters:** `:id` - The unique ID for the script.
    *   **Request Body:** JSON object:
        ```json
        {
          "name": "Script Name",
          "content": "# Script content here..."
        }
        ```
    *   **Response:**
        *   `200 OK` on success, returning the saved script data:
            ```json
            {
              "success": true,
              "script": {
                "id": "script-id-1",
                "name": "Script Name",
                "content": "# Script content here...",
                "lastModified": "2023-10-27T10:05:00.000Z"
              }
            }
            ```
        *   `400 Bad Request` if the request body is invalid (missing fields, not JSON).
        *   `500 Internal Server Error` if saving to S3 fails.

## Storage Details

*   **Index:** A single JSON file named `scripts.json` is stored at the root of the S3 bucket. It contains an array of `{id, name}` objects for all scripts. This file is overwritten every time a script is saved via the `PUT` endpoint.
*   **Scripts:** Each individual script is stored as a separate JSON object within a `scripts/` prefix (folder) in the bucket. The filename is `<script-id>.json`. This file contains the full script details (`id`, `name`, `content`, `lastModified`).