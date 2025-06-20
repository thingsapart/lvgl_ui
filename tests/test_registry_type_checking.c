#include "../registry.h" // Adjust path if necessary
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main() {
    Registry* reg = registry_create();
    if (!reg) {
        fprintf(stderr, "Failed to create registry for test.\n");
        return 1;
    }

    printf("Starting registry type checking tests...\n");

    // Test data
    int val1 = 1, val2 = 2;
    registry_add_pointer(reg, &val1, "id1", "lv_obj_t*");
    registry_add_pointer(reg, &val2, "id2", "lv_style_t*");
    registry_add_pointer(reg, NULL, "id3_null_ptr", "lv_font_t*");

    // Test 1: registry_get_pointer - correct type
    printf("Test 1: registry_get_pointer with correct type (no warning expected for id1)...\n");
    registry_get_pointer(reg, "id1", "lv_obj_t*");

    // Test 2: registry_get_pointer - incorrect type
    printf("Test 2: registry_get_pointer with incorrect type (WARNING EXPECTED for id1 type mismatch)...\n");
    registry_get_pointer(reg, "id1", "lv_style_t*");

    // Test 3: registry_get_pointer - different incorrect type
    printf("Test 3: registry_get_pointer with another incorrect type (WARNING EXPECTED for id2 type mismatch)...\n");
    registry_get_pointer(reg, "id2", "other_type*");

    // Test 4: registry_get_pointer - non-existent ID
    printf("Test 4: registry_get_pointer with non-existent ID (no warning expected)...\n");
    registry_get_pointer(reg, "non_existent_id", "lv_obj_t*");

    // Test 5: registry_get_pointer - NULL expected_type
    printf("Test 5: registry_get_pointer with NULL expected_type (no warning expected for id1)...\n");
    registry_get_pointer(reg, "id1", NULL);

    // Test 6: registry_get_pointer_by_id - existing ID
    printf("Test 6: registry_get_pointer_by_id for id1...\n");
    const char* type_out1 = NULL;
    void* ptr1 = registry_get_pointer_by_id(reg, "id1", &type_out1);
    assert(ptr1 == &val1 && type_out1 != NULL && strcmp(type_out1, "lv_obj_t*") == 0);
    printf("  ptr1: %p, type_out1: %s\n", ptr1, type_out1 ? type_out1 : "NULL");

    // Test 7: registry_get_pointer_by_id - non-existent ID
    printf("Test 7: registry_get_pointer_by_id for non_existent_id...\n");
    const char* type_out_non = NULL;
    void* ptr_non = registry_get_pointer_by_id(reg, "non_existent_id", &type_out_non);
    assert(ptr_non == NULL && type_out_non == NULL);
    printf("  ptr_non: %p, type_out_non: %s\n", ptr_non, type_out_non ? type_out_non : "NULL");

    // Test 8: registry_get_pointer_by_id - NULL type_out pointer
    printf("Test 8: registry_get_pointer_by_id for id2 with NULL type_out...\n");
    void* ptr2_null_type_out = registry_get_pointer_by_id(reg, "id2", NULL);
    assert(ptr2_null_type_out == &val2);
    printf("  ptr2_null_type_out: %p\n", ptr2_null_type_out);

    // Test 9: registry_get_type_by_id - existing ID
    printf("Test 9: registry_get_type_by_id for id2...\n");
    const char* type_str2 = registry_get_type_by_id(reg, "id2");
    assert(type_str2 != NULL && strcmp(type_str2, "lv_style_t*") == 0);
    printf("  type_str2: %s\n", type_str2 ? type_str2 : "NULL");

    // Test 10: registry_get_type_by_id - non-existent ID
    printf("Test 10: registry_get_type_by_id for non_existent_id...\n");
    const char* type_str_non = registry_get_type_by_id(reg, "non_existent_id");
    assert(type_str_non == NULL);
    printf("  type_str_non: %s\n", type_str_non ? type_str_non : "NULL");

    // Test 11: registry_get_type_by_id for id with NULL ptr
    printf("Test 11: registry_get_type_by_id for id3_null_ptr...\n");
    const char* type_str_id3 = registry_get_type_by_id(reg, "id3_null_ptr");
    assert(type_str_id3 != NULL && strcmp(type_str_id3, "lv_font_t*") == 0);
    printf("  type_str_id3: %s\n", type_str_id3 ? type_str_id3 : "NULL");


    registry_free(reg);
    printf("Registry type checking tests finished. Manually check stderr for expected warnings for tests 2 and 3.\n");
    return 0;
}
