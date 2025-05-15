// micropatterns_emulator/compiled_runtime.js

export class MicroPatternsCompiledRunner {
    constructor(ctx, errorCallback) {
        this.ctx = ctx;
        this.drawing = new MicroPatternsDrawing(ctx); // Shares the existing drawing library
        this.errorCallback = errorCallback || ((msg) => { console.error("CompiledRunner Error:", msg); });
    }

    resetDisplay() {
        this.ctx.fillStyle = 'white';
        this.ctx.fillRect(0, 0, this.drawing.display_width, this.drawing.display_height);
        // Reset drawing state if it's managed here, or ensure compiled function does it.
        // For now, compiled function initializes its own _state.
    }

    execute(compiledOutput, assets, environment) {
        this.resetDisplay();
        try {
            if (typeof compiledOutput.execute !== 'function') {
                throw new Error("Compiled output does not contain an executable function.");
            }
            // The compiledOutput.execute function will manage its own state
            // and use the passed 'drawing' interface.
            // It expects: _environment, _drawing, _assets, _initialUserVariables
            compiledOutput.execute(
                environment,      // _environment
                this.drawing,     // _drawing
                assets,           // _assets (this should be assets.assets from parser)
                compiledOutput.initialVariables // _initialUserVariables
            );
        } catch (e) {
            if (e.isCompiledRuntimeError) {
                this.errorCallback(e.message); // Error message already formatted by _runtimeError
            } else {
                // Catch unexpected JS errors from the compiled function itself
                this.errorCallback(`Unexpected Compiled Script Error: ${e.message}`);
                console.error("Unexpected Compiled Script Error Stack:", e.stack);
            }
        }
    }
}