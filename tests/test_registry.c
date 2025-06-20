#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../registry.h" // Assuming registry.h is in the parent directory

// --- Test Runner Helper ---
void run_test(void (*test_func)(void), const char* test_name) {
    printf("Running test: %s...\n", test_name);
    test_func();
    printf("Test %s PASSED\n", test_name);
}

// --- Test Cases ---

void test_string_registry() {
    Registry* reg = registry_create();
    assert(reg != NULL);

    // Add a new string
    const char* s1 = registry_add_str(reg, "hello");
    assert(s1 != NULL && "s1 should not be NULL");
    assert(strcmp(s1, "hello") == 0 && "s1 should be 'hello'");

    // Add the same string again
    const char* s2 = registry_add_str(reg, "hello");
    assert(s2 != NULL && "s2 should not be NULL");
    assert(s1 == s2 && "s1 and s2 should be the same pointer for identical strings");

    // Add a different string
    const char* s3 = registry_add_str(reg, "world");
    assert(s3 != NULL && "s3 should not be NULL");
    assert(strcmp(s3, "world") == 0 && "s3 should be 'world'");
    assert(s3 != s1 && "s3 should be different from s1");

    // Add an empty string
    const char* s4 = registry_add_str(reg, "");
    assert(s4 != NULL && "s4 should not be NULL");
    assert(strcmp(s4, "") == 0 && "s4 should be an empty string");

    // Test adding NULL (expect NULL back or specific error handling if defined)
    // Based on current registry.c, it prints an error and returns NULL.
    const char* s5 = registry_add_str(reg, NULL);
    assert(s5 == NULL && "registry_add_str with NULL should return NULL");

    // Test adding another string after NULL test
    const char* s6 = registry_add_str(reg, "another");
    assert(s6 != NULL && "s6 should not be NULL");
    assert(strcmp(s6, "another") == 0 && "s6 should be 'another'");


    registry_free(reg);
}

void test_pointer_registry() {
    Registry* reg = registry_create();
    assert(reg != NULL);

    int val1 = 10, val2 = 20, val3 = 30, val4 = 40;
    void* p1 = &val1;
    void* p2 = &val2;
    void* p3 = &val3;
    void* p4 = &val4; // For NULL type test

    // Add pointers
    registry_add_pointer(reg, p1, "id1", "typeA");
    registry_add_pointer(reg, p2, "id2", "typeA");
    registry_add_pointer(reg, p3, "id1", "typeB"); // Same ID, different type

    // Test registry_get_pointer
    void* rp1 = registry_get_pointer(reg, "id1", "typeA");
    assert(rp1 == p1 && "Failed to get p1 by id1/typeA");

    void* rp2 = registry_get_pointer(reg, "id2", "typeA");
    assert(rp2 == p2 && "Failed to get p2 by id2/typeA");

    void* rp3 = registry_get_pointer(reg, "id1", "typeB");
    assert(rp3 == p3 && "Failed to get p3 by id1/typeB");

    // Test with type NULL: registry_get_pointer should return the most recently added pointer matching the ID.
    // "id1", "typeB" (p3) was added after "id1", "typeA" (p1).
    // Linked list prepends, so "id1", "typeB" will be found first.
    void* rp4 = registry_get_pointer(reg, "id1", NULL);
    assert(rp4 == p3 && "Failed to get most recent p3 for id1 with type NULL");

    // Test non-existent
    void* rpn1 = registry_get_pointer(reg, "id_unknown", "typeA");
    assert(rpn1 == NULL && "Should get NULL for unknown id");

    void* rpn2 = registry_get_pointer(reg, "id1", "type_unknown");
    assert(rpn2 == NULL && "Should get NULL for unknown type with known id");

    // Test adding with NULL type
    registry_add_pointer(reg, p4, "id_null_type", NULL);

    void* rp5 = registry_get_pointer(reg, "id_null_type", NULL);
    assert(rp5 == p4 && "Failed to get pointer added with NULL type when querying with NULL type");

    // If a pointer is added with type NULL, querying it with a specific type should not find it.
    void* rp6 = registry_get_pointer(reg, "id_null_type", "some_type");
    assert(rp6 == NULL && "Should not find pointer added with NULL type when querying with a specific type");

    // Test adding another pointer with same ID but different non-NULL type
    registry_add_pointer(reg, p1, "id_null_type", "typeSpecific");
    void* rp7 = registry_get_pointer(reg, "id_null_type", "typeSpecific");
    assert(rp7 == p1 && "Failed to get specific type after adding NULL type with same ID");

    // The NULL type entry should still be findable with type NULL
    // (p4 was added with "id_null_type", NULL. p1 was added with "id_null_type", "typeSpecific")
    // p1 is now head of list for "id_null_type"
    void* rp8 = registry_get_pointer(reg, "id_null_type", NULL);
    assert(rp8 == p1 && "When type is NULL, most recent ID match should be p1");


    // Test adding NULL for id or ptr (should be handled gracefully, e.g. no-op or error print)
    // Based on registry.c, these print an error and return.
    registry_add_pointer(reg, NULL, "id_null_ptr", "typeC"); // ptr is NULL
    assert(registry_get_pointer(reg, "id_null_ptr", "typeC") == NULL && "Should not register NULL ptr");

    registry_add_pointer(reg, p1, NULL, "typeD"); // id is NULL
    // This case is not explicitly guarded in registry_add_pointer, could lead to crash if strdup(NULL).
    // Current registry.c: `if (!reg || !id || !ptr)` - this will catch it.
    // No easy way to assert it wasn't added without knowing if other things would crash.
    // Assuming the guard in registry_add_pointer works, it just returns.

    registry_free(reg);
}


int main() {
    printf("Starting registry tests...\n");

    run_test(test_string_registry, "test_string_registry");
    run_test(test_pointer_registry, "test_pointer_registry");

    printf("All registry tests completed.\n");
    return 0;
}
