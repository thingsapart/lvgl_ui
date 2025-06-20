#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "cJSON.h"
#include "registry.h"
#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "utils.h"     // For read_file

// --- Test Runner Helper ---
void run_gen_reg_ptr_test(void (*test_func)(void), const char* test_name) {
    printf("Running generator pointer registry test: %s...\n", test_name);
    test_func();
    printf("Test %s PASSED\n", test_name);
}

// --- Helper Functions ---
static cJSON* parse_json_string(const char* json_str) {
    cJSON* json = cJSON_Parse(json_str);
    assert(json != NULL && "Failed to parse JSON string");
    return json;
}

void test_generator_pointer_registration_ir() {
    // Load the MINIMAL API spec from file
    char* api_spec_file_content = read_file("tests/minimal_api_spec_for_ids.json");
    assert(api_spec_file_content != NULL && "Failed to read API spec file tests/minimal_api_spec_for_ids.json");
    cJSON* api_spec_json = cJSON_Parse(api_spec_file_content);
    assert(api_spec_json != NULL && "Failed to parse API spec JSON");
    free(api_spec_file_content);
    ApiSpec* api_spec = api_spec_parse(api_spec_json);
    assert(api_spec != NULL && "Failed to parse ApiSpec from cJSON");

    const char* ui_json_str =
        "[\n"
        "  { \"type\": \"obj\", \"id\": \"@myTestObj\", \"named\": \"c_my_test_obj\" }\n"
        "]";

    cJSON* ui_json_root = parse_json_string(ui_json_str);

    IRStmtBlock* ir_block = generate_ir_from_ui_spec(ui_json_root, api_spec);
    assert(ir_block != NULL && "IR generation failed");

    printf("--- Generated IR for Pointer Test ---\n");
    IRStmtNode* stmt_node_iter;
    // The generator typically creates a block for each top-level JSON object.
    // So we iterate through those blocks, then their statements.
    for (stmt_node_iter = ir_block->stmts; stmt_node_iter; stmt_node_iter = stmt_node_iter->next) {
        IRStmt* current_stmt = stmt_node_iter->stmt;
        if (!current_stmt) continue;

        if (current_stmt->type == IR_STMT_BLOCK) {
            printf("  Block found:\n");
            IRStmtNode* sub_stmt_node_iter;
            for (sub_stmt_node_iter = ((IRStmtBlock*)current_stmt)->stmts; sub_stmt_node_iter; sub_stmt_node_iter = sub_stmt_node_iter->next) {
                IRStmt* sub_stmt = sub_stmt_node_iter->stmt;
                if (!sub_stmt) continue;

                if (sub_stmt->type == IR_STMT_WIDGET_ALLOCATE) {
                     IRStmtWidgetAllocate* alloc_stmt = (IRStmtWidgetAllocate*)sub_stmt;
                     printf("    Widget Alloc: %s (type %s, func %s, parent: %s)\n",
                            alloc_stmt->c_var_name,
                            alloc_stmt->widget_c_type_name,
                            alloc_stmt->create_func_name,
                            alloc_stmt->parent_expr && alloc_stmt->parent_expr->type == IR_EXPR_VARIABLE ? ((IRExprVariable*)alloc_stmt->parent_expr)->name : "NULL_OR_NON_VAR"
                           );
                } else if (sub_stmt->type == IR_STMT_OBJECT_ALLOCATE) {
                     IRStmtObjectAllocate* obj_alloc_stmt = (IRStmtObjectAllocate*)sub_stmt;
                     printf("    Object Alloc: %s (type %s, func %s)\n",
                            obj_alloc_stmt->c_var_name,
                            obj_alloc_stmt->object_c_type_name,
                            obj_alloc_stmt->init_func_name
                           );
                } else if (sub_stmt->type == IR_STMT_FUNC_CALL) {
                    IRStmtFuncCall* call_stmt = (IRStmtFuncCall*)sub_stmt;
                    printf("    Func Call: %s(", call_stmt->call->func_name);
                    IRExprNode* arg_iter;
                    for (arg_iter = call_stmt->call->args; arg_iter; arg_iter = arg_iter->next) {
                        if (!arg_iter->expr) { printf("NULL_EXPR"); continue; }
                        switch (arg_iter->expr->type) {
                            case IR_EXPR_VARIABLE: printf("%s", ((IRExprVariable*)arg_iter->expr)->name); break;
                            case IR_EXPR_LITERAL: printf("%s", ((IRExprLiteral*)arg_iter->expr)->value); break;
                            // Note: ir_new_literal_string creates an IR_EXPR_LITERAL.
                            // If it were a distinct IR_EXPR_LITERAL_STRING, that case would be needed.
                            default: printf("..."); break;
                        }
                        if (arg_iter->next) printf(", ");
                    }
                    printf(")\n");
                } else {
                    printf("    Other Stmt type: %d\n", sub_stmt->type);
                }
            }
        } else {
             printf("  Top-level Stmt type (unexpected for this test): %d\n", current_stmt->type);
        }
    }
    printf("--- End of IR ---\n");
    fflush(stdout); // Ensure all prints are out before assertions

    int registry_add_pointer_calls = 0;
    int myTestObj_found = 0;

    // Reset iterator for actual assertion pass
    for (stmt_node_iter = ir_block->stmts; stmt_node_iter; stmt_node_iter = stmt_node_iter->next) {
        IRStmt* current_stmt = stmt_node_iter->stmt;
        if (!current_stmt || current_stmt->type != IR_STMT_BLOCK) continue;

        IRStmtNode* sub_stmt_node_iter;
        for (sub_stmt_node_iter = ((IRStmtBlock*)current_stmt)->stmts; sub_stmt_node_iter; sub_stmt_node_iter = sub_stmt_node_iter->next) {
            IRStmt* sub_stmt = sub_stmt_node_iter->stmt;
            if (!sub_stmt || sub_stmt->type != IR_STMT_FUNC_CALL) continue;

            IRStmtFuncCall* func_call_stmt = (IRStmtFuncCall*)sub_stmt;
            if (strcmp(func_call_stmt->call->func_name, "registry_add_pointer") == 0) {
                registry_add_pointer_calls++;

                IRExprNode* args = func_call_stmt->call->args;
                assert(args && args->expr->type == IR_EXPR_VARIABLE && "Arg1: ui_registry (var)");
                assert(strcmp(((IRExprVariable*)args->expr)->name, "ui_registry") == 0);

                args = args->next;
                assert(args && args->expr->type == IR_EXPR_VARIABLE && "Arg2: C var name (var)");
                const char* c_var_name = ((IRExprVariable*)args->expr)->name;

                args = args->next;
                assert(args && args->expr->type == IR_EXPR_LITERAL && "Arg3: JSON ID (literal)");
                const char* json_id = ((IRExprLiteral*)args->expr)->value;

                args = args->next;
                assert(args && args->expr->type == IR_EXPR_LITERAL && "Arg4: JSON type (literal)");
                const char* json_type = ((IRExprLiteral*)args->expr)->value;
                assert(args->next == NULL && "Should only have 4 arguments");

                printf("Asserting: json_id ('%s') == 'myTestObj'\n", json_id);
                printf("Asserting: c_var_name ('%s') == 'c_my_test_obj'\n", c_var_name);
                printf("Asserting: json_id ('%s') == 'myTestObj'\n", json_id);
                printf("Asserting: c_var_name ('%s') == 'c_my_test_obj'\n", c_var_name);
                printf("Asserting: json_id ('%s') == 'myTestObj'\n", json_id);
                printf("Asserting: c_var_name ('%s') == 'c_my_test_obj'\n", c_var_name);
                printf("Asserting: json_type ('%s') == 'obj'\n", json_type);

                bool id_ok = strcmp(json_id, "myTestObj") == 0;
                bool c_var_ok = strcmp(c_var_name, "c_my_test_obj") == 0;
                bool type_ok = strcmp(json_type, "obj") == 0;

                printf("Result of id_ok: %d, c_var_ok: %d, type_ok: %d\n", id_ok, c_var_ok, type_ok);

                if (id_ok) {
                    if (!c_var_ok) {
                        printf("ERROR: C_VAR_NAME MISMATCH! Expected 'c_my_test_obj', got '%s'\n", c_var_name);
                    }
                    if (!type_ok) {
                        printf("ERROR: JSON_TYPE MISMATCH! Expected 'obj', got '%s'\n", json_type);
                    }

                    if (c_var_ok && type_ok) {
                        myTestObj_found++;
                        printf("myTestObj_found incremented to %d\n", myTestObj_found);
                    } else {
                        printf("Skipping myTestObj_found increment due to c_var or type mismatches.\n");
                    }
                } else {
                     printf("ERROR: JSON_ID MISMATCH! Expected 'myTestObj', got '%s'\n", json_id);
                }
            }
        }
    }

    printf("Final check: registry_add_pointer_calls = %d, myTestObj_found = %d\n", registry_add_pointer_calls, myTestObj_found);
    assert(registry_add_pointer_calls == 1 && "Expected 1 call to registry_add_pointer");
    assert(myTestObj_found == 1 && "registry_add_pointer for myTestObj not found or arguments incorrect");

    ir_free((IRNode*)ir_block);
    cJSON_Delete(ui_json_root);
    api_spec_free(api_spec);
    cJSON_Delete(api_spec_json);
}


int main() {
    printf("Starting generator pointer registry tests...\n");
    run_gen_reg_ptr_test(test_generator_pointer_registration_ir, "test_generator_pointer_registration_ir");
    printf("All generator pointer registry tests completed.\n");
    return 0;
}
