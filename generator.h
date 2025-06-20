#ifndef GENERATOR_H
#define GENERATOR_H

#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include <cJSON/cJSON.h>

// Main function to generate IR from UI spec.
// Creates and manages its own GenContext and Registry for string deduplication.
IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec);

// Function to generate IR, allowing an external registry to be used by GenContext (for testing string deduplication).
// If string_registry_for_gencontext is NULL, it behaves like generate_ir_from_ui_spec regarding registry creation/destruction.
IRStmtBlock* generate_ir_from_ui_spec_with_registry(
    const cJSON* ui_spec_root,
    const ApiSpec* api_spec,
    Registry* string_registry_for_gencontext // Used by GenContext for registry_add_str calls
);

#endif // GENERATOR_H
