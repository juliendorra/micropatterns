import { nanoid } from "https://esm.sh/nanoid@5.1.5/es2022/nanoid.mjs";

/**
 * Generates a 21-character unique ID for publishing.
 *
 * @returns A 21-character unique string.
 */
export function generatePublishId(): string {
  return nanoid(21);
}

// Conceptual test: Log a generated ID to the console
console.log("Generated Publish ID:", generatePublishId());
