//
// Console string constants shared by the boot path.
//
// Prefixed ES_ because these are unqualified all-caps names in a header: an
// unprefixed ERROR in particular collides with vendor and system headers that
// define the same identifier.
//

#ifndef ES2025_CONSTANTS_H
#define ES2025_CONSTANTS_H

#define ES_DEBUG_TITLE        "[[DEBUG]]"
#define ES_ERROR              "[[ERROR]]"

#define ES_STARTING_TITLE     "Starting"
#define ES_CREATE_FAIL        "Failed to create"

#define ES_SENSOR_TASK_TITLE  "[[SENSOR TASK]]"

#endif //ES2025_CONSTANTS_H
