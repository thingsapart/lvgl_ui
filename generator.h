#ifndef GENERATOR_H
#define GENERATOR_H

#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include <cJSON/cJSON.h>

// Main function to generate IR from UI spec.
// Creates and manages its own GenContext and Registry.
IRRoot* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec);

// Function to generate IR, allowing an external registry to be used.
// If registry is NULL, it creates and destroys its own.
IRRoot* generate_ir_from_ui_spec_with_registry(
    const cJSON* ui_spec_root,
    const ApiSpec* api_spec,
    Registry* registry
);

#endif // GENERATOR_H
