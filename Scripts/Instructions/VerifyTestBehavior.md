Do not use instructions from this file unless asked.

# Verify Test Behavior

This instruction walks through Untested cases in the codebase, presenting each test's behavior in human-readable detail so the user can verify that the tests match the intended design. It covers discovering tests, explaining what each test does, and flagging mismatches for follow-up.

## Prerequisites

- The `Source/` directory must be accessible
- The user should have context on which subsystem or area they want to verify (e.g., "components", "subsystems", "utilities"), or they can request all categories

## Step 1: Gather Scope

1. If the user specified a subsystem or area of interest, use that to filter the search
2. If no area was specified, ask the user:
   > "Which area would you like to verify test behavior for? (e.g., components, subsystems, utilities, networking — or 'all' to see every category)"
3. Record the user's chosen scope for filtering in the next step

## Step 2: Index Untested Cases

1. Search `.cpp` files under `Source/` for Untested macros. The macro patterns to match are:
   - `UNTEST_UNIT`, `UNTEST_UNIT_F`, `UNTEST_UNIT_PURE`
   - `UNTEST_WORLD`, `UNTEST_WORLD_F`, `UNTEST_WORLD_F_OPTS`
   - `UNTEST_CLIENTSERVER`, `UNTEST_CLIENTSERVER_F`, `UNTEST_CLIENTSERVER_F_OPTS`

2. Test files are typically located at: `Source/*/Private/**/Tests/*.cpp`

3. For each match, parse the test identifier triple: `Module.Category.TestName`
   - Example: `UNTEST_UNIT(MyModule, ComponentBehavior, AddStackableIncrementsQuantity)` → `MyModule.ComponentBehavior.AddStackableIncrementsQuantity`

4. Group tests by `Module.Category`

## Step 3: Present Categories

1. Filter the discovered categories to match the user's area of interest:
   - Use case-insensitive substring matching against category names
   - If user said "all", include every category

2. Present the matching categories as a numbered list:
   ```
   Found 3 categories matching "components":
   1. MyModule.ComponentBehavior (5 tests)
   2. MyModule.ComponentStacking (3 tests)
   3. MyModule.ComponentUpgrade (4 tests)
   ```

3. Ask the user to pick one or more categories (or "all matching")

## Step 4: Present Test Cases

1. For the chosen category/categories, list all test cases:
   ```
   MyModule.ComponentBehavior:
   1. AddStackableIncrementsQuantity
   2. RemoveNonStackableDeletesEntry
   3. SlotRejectsWrongType
   ...
   A. All of the above
   ```

2. Ask the user which tests to walk through (specific numbers or "all")

## Step 5: Walk Through Each Selected Test

For each selected test, perform the following:

### 5a. Read the Test Source

Read the full test function body from the source file.

### 5b. Describe the Behavior

Present a plain-English summary structured as:

- **Setup**: What objects/state are created before the test action (e.g., "Creates a component with 10 slots and adds a stackable item definition")
- **Action**: What operation is being tested (e.g., "Calls AddItem with the stackable item and quantity 5")
- **Assertion**: What the test verifies (e.g., "Asserts that the item's stack count is now 5 and only one slot is occupied")

### 5c. Ask for Confirmation

Ask the user:
> "Does this match the intended design behavior for **TestName**?"

### 5d. Handle Mismatch

If the user says **no** (the test does not match intended design):

1. Ask the user to describe the expected behavior
2. Offer to create a task to fix the test, or note it for later resolution
3. If the user declines to fix it now, record it as "needs fix" and continue to the next test

If the user says **yes**, record it as "confirmed" and continue.

If the user wants to **skip**, record it as "skipped" and continue.

## Step 6: Summary

After walking through all selected tests, present a summary table:

```
## Test Verification Summary

Category: MyModule.ComponentBehavior

| # | Test Name                      | Status     | Notes                    |
|---|--------------------------------|------------|--------------------------|
| 1 | AddStackableIncrementsQuantity | Confirmed  |                          |
| 2 | RemoveNonStackableDeletesEntry | Needs Fix  | Should preserve metadata |
| 3 | SlotRejectsWrongType           | Skipped    |                          |

Results: 1 confirmed, 1 needs fix, 1 skipped
```

If any tests were flagged as "needs fix", remind the user of the options:
- Create a task to fix the test
- Address them later

## Error Handling

- **No test files found**: Report that no Untested macros were found under `Source/`. Suggest verifying the search path or checking if tests exist for the requested area.
- **No categories match the user's area**: Show all available categories and ask the user to pick from the full list.
- **Cannot parse test macro**: Skip the malformed entry, warn the user, and continue with remaining tests.
- **File read failure**: Report the file path that failed and continue with other tests.

## Important Notes

- **Read-only operation**: This instruction only reads and explains tests. It does not modify any code unless the user explicitly asks to fix a test.
- **Test macro variants**: The `_F` suffix indicates a test fixture class. `_OPTS` variants accept additional options. `_PURE` tests have no Unreal dependencies. All variants follow the same `Module.Category.TestName` pattern.
- **Large test suites**: If a category has many tests (>20), consider suggesting the user work through them in batches to avoid fatigue.

## Example Execution

### Example 1: Verifying Component Tests

User says: "verify test behavior for components"

```
Scanning Source/ for Untested macros...

Found 3 categories matching "components":
1. MyModule.ComponentStacking (4 tests)
2. MyModule.ComponentUpgrade (3 tests)
A. All matching categories

Which categories would you like to verify?
```

User picks "1":

```
MyModule.ComponentStacking:
1. AddStackableIncrementsQuantity
2. AddBeyondMaxStackCreatesNewSlot
3. RemovePartialStackDecrementsQuantity
4. MergeStacksCombinesQuantity
A. All of the above

Which tests would you like to walk through?
```

User picks "A", then for each test:

```
## Test: AddStackableIncrementsQuantity

- **Setup**: Creates a component and registers a stackable item definition with max stack size 99
- **Action**: Calls AddItem twice with quantities 10 and 15
- **Assertion**: Verifies the single stack has quantity 25 and only one slot is used

Does this match the intended design behavior for AddStackableIncrementsQuantity?
```

### Example 2: Mismatch Found

User says "no, stacking should cap at 20 and overflow into a new slot":

```
Noted: AddStackableIncrementsQuantity does not match intended design.
Expected: Adding beyond stack cap (20) should create a new slot with the overflow.

Options:
  1. Create a task to fix this test now
  2. Note it for later and continue

Continuing to next test...
```
