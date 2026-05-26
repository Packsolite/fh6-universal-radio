import js from "@eslint/js";
import globals from "globals";

export default [
  { ignores: ["build/**", "dist/**", "third_party/**", "node_modules/**"] },
  js.configs.recommended,
  {
    files: ["ui/dist/**/*.js"],
    languageOptions: {
      ecmaVersion: 2023,
      sourceType: "module",
      globals: { ...globals.browser },
    },
    rules: {
      "no-unused-vars": ["warn", { argsIgnorePattern: "^_", varsIgnorePattern: "^_" }],
      "no-shadow": "warn",
      "no-implicit-globals": "error",
      "no-var": "error",
      "prefer-const": "warn",
      "eqeqeq": ["error", "smart"],
      "curly": ["error", "multi-line"],
      "no-console": "off",
    },
  },
];
