#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "cJSON.h"
#include "registry.h"
#include "api_spec.h"
#include "generator.h" // For generate_ir_from_ui_spec_with_registry
#include "ir.h"        // For IR node types and walking
#include "utils.h"     // For read_file

// --- Test Runner Helper ---
void run_gen_reg_test(void (*test_func)(void), const char* test_name) {
    printf("Running generator registry test: %s...\n", test_name);
    test_func();
    printf("Test %s PASSED\n", test_name);
}

// --- Helper Functions ---
static cJSON* parse_json_string(const char* json_str) {
    cJSON* json = cJSON_Parse(json_str);
    assert(json != NULL && "Failed to parse JSON string");
    return json;
}

// REMOVE: create_minimal_api_spec and its helpers add_minimal_property, add_minimal_widget
// These functions (add_minimal_property, add_minimal_widget) are no longer used.

// --- Test Cases ---

void test_generator_string_deduplication() {
    // Test with GENERATOR_REGISTRY_TEST_BYPASS_APISPEC_FIND_FOR_STRINGS define
    // ApiSpec can be minimal or NULL if generator handles it.
    ApiSpec* api_spec = (ApiSpec*)calloc(1, sizeof(ApiSpec)); // Dummy ApiSpec
    assert(api_spec != NULL && "Failed to allocate dummy ApiSpec");

    const char* ui_json_str =
        "[\n"
        "  {\n"
        "    \"type\": \"label\", \"named\": \"label1\",\n"
        "    \"properties\": { \"text\": \"!Hello World\", \"other_prop\": \"!Foo\" }\n"
        "  },\n"
        "  {\n"
        "    \"type\": \"button\", \"named\": \"button1\",\n"
        "    \"properties\": { \"text\": \"!Hello World\", \"another_prop\": \"!Bar\" }\n"
        "  }\n"
        "]";
    cJSON* ui_json_root = parse_json_string(ui_json_str);
    Registry* test_registry = registry_create();
    assert(test_registry != NULL && "Test registry creation failed");

    IRStmtBlock* ir_block = generate_ir_from_ui_spec_with_registry(ui_json_root, api_spec, test_registry);
    assert(ir_block != NULL && "IR generation failed");

    int string_count = 0;
    StringRegistryNode* current_s_node = test_registry->strings;
    const char* str_hello = NULL;
    const char* str_foo = NULL;
    const char* str_bar = NULL;

    // Debug prints (optional, can be removed if test passes)
    printf("--- Debugging test_generator_string_deduplication (bypass active) ---\n");
    StringRegistryNode* debug_node = test_registry->strings;
    while(debug_node) {
        printf("  Registry String: '%s'\n", debug_node->value);
        debug_node = debug_node->next;
    }

    while (current_s_node != NULL) {
        string_count++;
        if (strcmp(current_s_node->value, "Hello World") == 0) str_hello = current_s_node->value;
        if (strcmp(current_s_node->value, "Foo") == 0) str_foo = current_s_node->value;
        if (strcmp(current_s_node->value, "Bar") == 0) str_bar = current_s_node->value;
        current_s_node = current_s_node->next;
    }
    printf("Final string_count = %d. Hello: %p, Foo: %p, Bar: %p\n", string_count, str_hello, str_foo, str_bar);
    printf("--- End Debugging ---\n");

    assert(string_count == 3 && "Expected 3 unique strings in the registry with bypass");
    assert(str_hello != NULL && "String 'Hello World' not found in registry");
    assert(str_foo != NULL && "String 'Foo' not found in registry");
    assert(str_bar != NULL && "String 'Bar' not found in registry");
    // printf("String test assertions commented out to see debug prints.\n");

    // Cleanup
    ir_free((IRNode*)ir_block);
    cJSON_Delete(ui_json_root);
    if (api_spec) free(api_spec); // Free dummy ApiSpec
    registry_free(test_registry);
}

// test_generator_pointer_registration_ir has been moved to tests/test_generator_registry_pointers.c

int main() {
    printf("Starting generator string registry integration tests...\n"); // Changed title
    run_gen_reg_test(test_generator_string_deduplication, "test_generator_string_deduplication");
    printf("All generator string registry tests completed.\n"); // Changed title
    return 0;
}
