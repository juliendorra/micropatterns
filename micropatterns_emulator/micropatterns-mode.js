// CodeMirror mode for MicroPatterns DSL v1.0 (Unified Asset Update)

(function(mod) {
  if (typeof exports == "object" && typeof module == "object") // CommonJS
    mod(require("../../lib/codemirror"));
  else if (typeof define == "function" && define.amd) // AMD
    define(["../../lib/codemirror"], mod);
  else // Plain browser env
    mod(CodeMirror);
})(function(CodeMirror) {
"use strict";

CodeMirror.defineMode("micropatterns", function() {

  // Case-insensitive keywords
  // Updated: DEFINE, PATTERN (as part of DEFINE), FILL, DRAW. Removed ICON. Added FILL_PIXEL.
  // Removed TIMES from keywords as it's no longer part of REPEAT syntax.
  const keywords = /^(?:DEFINE|PATTERN|VAR|LET|COLOR|FILL|DRAW|RESET_TRANSFORMS|TRANSLATE|ROTATE|SCALE|PIXEL|FILL_PIXEL|LINE|RECT|FILL_RECT|CIRCLE|FILL_CIRCLE|REPEAT|IF|THEN|ELSE|ENDIF|ENDREPEAT|BLACK|WHITE|SOLID)\b/i;
  // Case-insensitive parameter names followed by =
  const properties = /^(?:NAME|WIDTH|HEIGHT|DATA|X|Y|X1|Y1|X2|Y2|DX|DY|DEGREES|FACTOR|RADIUS|COUNT)\s*=/i;
  // Case-insensitive environment variables
  const envVariables = /^(?:\$HOUR|\$MINUTE|\$SECOND|\$COUNTER|\$WIDTH|\$HEIGHT|\$INDEX)\b/i;
  // User-defined variables ($ followed by identifier) - case insensitive handled by regex flag
  const userVariables = /^\$[a-zA-Z_][a-zA-Z0-9_]*\b/i;
  // Operators
  const operators = /^(?:==?|!=|[<>]=?|[+\-*/%])/; // Includes assignment =

  function tokenBase(stream, state) {
    // Try comments first
    if (stream.peek() === '#') {
      stream.skipToEnd();
      return "comment";
    }

    // Eat whitespace
    if (stream.eatSpace()) return null;

    // Try keywords
    // Note: "PATTERN" is matched here, but contextually it's usually after DEFINE
    if (stream.match(keywords)) return "keyword";

    // Try parameter names (properties)
    if (stream.match(properties)) return "property"; // Style as property/attribute

    // Try environment variables
    if (stream.match(envVariables)) return "variable-2"; // Style as built-in variable

    // Try user variables
    if (stream.match(userVariables)) return "variable-3"; // Style as user-defined variable

    // Try numbers (integers, optional minus sign)
    if (stream.match(/^-?[0-9]+\b/)) return "number";

    // Try operators
    if (stream.match(operators)) return "operator";

    // Try strings (double quoted)
    if (stream.match(/^"(?:[^\\]|\\.)*?(?:"|$)/)) return "string";

    // If nothing else matches, advance the stream by one character
    stream.next();
    return null; // No specific style
  }

  return {
    startState: function() {
      return {tokenize: tokenBase};
    },
    token: function(stream, state) {
      return state.tokenize(stream, state);
    },
    lineComment: "#"
  };
});

CodeMirror.defineMIME("text/x-micropatterns", "micropatterns");

});