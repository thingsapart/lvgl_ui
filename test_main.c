#include <stdio.h>
#include <stdlib.h>
#include <cjson/cJSON.h> // Use system-installed cJSON
#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "codegen.h"
#include "utils.h"

int main() {
    char* api_spec_str = read_file("api_spec.json"); // Corrected function name
    if (!api_spec_str) {
        fprintf(stderr, "Failed to read api_spec.json\n");
        return 1;
    }
    cJSON* api_spec_json = cJSON_Parse(api_spec_str);
    if (!api_spec_json) {
        fprintf(stderr, "Failed to parse api_spec.json: %s\n", cJSON_GetErrorPtr());
        free(api_spec_str);
        return 1;
    }
    ApiSpec* api_spec = api_spec_parse(api_spec_json); // Corrected function name
    if (!api_spec) {
        fprintf(stderr, "Failed to load ApiSpec from cJSON\n");
        cJSON_Delete(api_spec_json); // Still delete if api_spec_parse fails
        free(api_spec_str);
        return 1;
    }
    free(api_spec_str);
    // Assuming api_spec_parse copies data or cJSON_Delete is handled by api_spec_free
    // For safety, api_spec_parse should not rely on api_spec_json after it returns.
    // cJSON_Delete(api_spec_json); // This can be problematic if api_spec uses it.
                                 // api_spec_free should handle its cJSON objects.

    char* ui_spec_str = read_file("tests/issue_spec.json"); // Corrected function name
    if (!ui_spec_str) {
        fprintf(stderr, "Failed to read tests/issue_spec.json\n");
        api_spec_free(api_spec);
        return 1;
    }
    cJSON* ui_spec_json = cJSON_Parse(ui_spec_str);
    if (!ui_spec_json) {
        fprintf(stderr, "Failed to parse tests/issue_spec.json: %s\n", cJSON_GetErrorPtr());
        free(ui_spec_str);
        api_spec_free(api_spec);
        return 1;
    }

    IRStmtBlock* ir_block = generate_ir_from_ui_spec(ui_spec_json, api_spec);
    if (!ir_block) {
        fprintf(stderr, "Failed to generate IR.\n");
        cJSON_Delete(ui_spec_json);
        free(ui_spec_str);
        api_spec_free(api_spec);
        return 1;
    }

    printf("// --- Generated C Code Output START --- \n");
    codegen_generate_c(ir_block, "parent"); // Corrected function name & used "parent" for parent_var_name
    printf("\n// --- Generated C Code Output END --- \n");

    ir_free((IRNode*)ir_block);
    cJSON_Delete(ui_spec_json);
    free(ui_spec_str);
    api_spec_free(api_spec);

    return 0;
}
