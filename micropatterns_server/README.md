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

All API endpoints now require a `userID` as part of the path to scope operations to a specific user. The `userID` is a secret, non-guessable ID generated by the client.

*   **`GET /api/scripts/:userID`**
    *   **Description:** Retrieves the list of available scripts for the specified `userID`. Reads from `<userID>.json` in the S3 bucket.
    *   **Parameters:** `:userID` - The secret user ID.
    *   **Response:** `200 OK` with a JSON array of script metadata (id, name, lastModified). Returns an empty array if the user has no scripts or the index doesn't exist.
        ```json
        [
          { "id": "script-id-1", "name": "Cool Pattern", "lastModified": "2023-10-27T10:00:00.000Z" },
          { "id": "script-id-2", "name": "Another One", "lastModified": "2023-10-28T11:00:00.000Z" }
        ]
        ```

*   **`GET /api/device/scripts/:userID`**
    *   **Description:** Retrieves the list of scripts selected for device synchronization for the specified `userID`. Reads from `<userID>-device.json`.
    *   **Parameters:** `:userID` - The secret user ID.
    *   **Response:** `200 OK` with a JSON array of script metadata. Returns an empty array if no scripts are selected or the index doesn't exist.
        ```json
        [
          { "id": "script-id-1", "name": "Cool Pattern", "lastModified": "2023-10-27T10:00:00.000Z" }
        ]
        ```

*   **`PUT /api/device/scripts/:userID`**
    *   **Description:** Updates the list of scripts selected for device synchronization for the specified `userID`. Overwrites `<userID>-device.json`.
    *   **Parameters:** `:userID` - The secret user ID.
    *   **Request Body:** JSON object:
        ```json
        {
          "selectedIds": ["script-id-1", "script-id-3"]
        }
        ```
    *   **Response:**
        *   `200 OK` on success.
        *   `400 Bad Request` if `selectedIds` is missing or not an array.
        *   `500 Internal Server Error` on failure.

*   **`GET /api/scripts/:userID/:scriptID`**
    *   **Description:** Retrieves the full data for a specific script belonging to the `userID`.
    *   **Parameters:**
        *   `:userID` - The secret user ID.
        *   `:scriptID` - The unique ID of the script.
    *   **Response:**
        *   `200 OK` with the script data.
        *   `404 Not Found` if the script or user doesn't exist.
            ```json
            {
              "id": "script-id-1",
              "name": "Cool Pattern",
              "content": "# MicroPatterns Script Content...\nCOLOR NAME=BLACK\n...",
              "lastModified": "2023-10-27T10:00:00.000Z"
            }
            ```

*   **`PUT /api/scripts/:userID/:scriptID`**
    *   **Description:** Saves a new script or updates an existing one for the specified `userID`. Saves to `scripts/<userID>/<scriptID>.json` and updates `<userID>.json`.
    *   **Parameters:**
        *   `:userID` - The secret user ID.
        *   `:scriptID` - The unique ID for the script.
    *   **Request Body:** JSON object:
        ```json
        {
          "name": "Script Name",
          "content": "# Script content here..."
        }
        ```
    *   **Response:**
        *   `200 OK` on success, returning the saved script data.
        *   `400 Bad Request` if the request body is invalid.
        *   `500 Internal Server Error` on failure.

## Storage Details

Storage is now namespaced by User ID:

*   **User Script Index:** A JSON file named `<userID>.json` (e.g., `kynsrxkpq8.json`) is stored at the root of the S3 bucket. It contains an array of `{id, name, lastModified}` objects for all scripts belonging to that user.
*   **User Device Sync Index:** A JSON file named `<userID>-device.json` (e.g., `kynsrxkpq8-device.json`) is stored at the root of the S3 bucket. It contains an array of script metadata for scripts selected for device sync by that user.
*   **Individual Scripts:** Each script is stored as a JSON object within a user-specific prefix: `scripts/<userID>/<scriptID>.json` (e.g., `scripts/kynsrxkpq8/cool-pattern.json`). This file contains the full script details (`id`, `name`, `content`, `lastModified`).