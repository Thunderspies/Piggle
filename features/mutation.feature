Feature: Physical deletion and format-aware archive editing

  @EDIT-001
  Scenario: Deleting the selected winner reveals the next physical copy
    Given "patch" wins "a" over "base"
    When I delete the selected patch copy and complete commitment
    Then "base" becomes visible as "a"
    And only the selected physical copy has been deleted
    And lookup no longer selects the deleted copy

  @EDIT-002
  Scenario: Deleting one colliding archive record reveals the next record
    Given one archive contains "A" as record 1 and "a" as record 2
    When I select and delete record 2
    Then record 1 becomes the selected copy of "a"
    And no public cursor exposes both records

  @EDIT-003
  Scenario Outline: Explicit source deletion removes its backing data
    Given an open detached <kind> source with no active transfers
    When I explicitly delete that source
    Then its <data> is removed
    And retained metadata handles become stale for content access
    Examples:
      | kind    | data                         |
      | archive | archive file                 |
      | loose   | directory and its descendants |

  @EDIT-004
  Scenario: HOGG edits use recoverable mutation facilities
    Given a writable HOGG contains an old selected copy
    When I replace it with complete validated content
    Then payload preparation precedes the journal commitment
    And the intended record is updated through HOGG mutation facilities
    And unrelated records and their metadata are preserved

  @EDIT-005
  Scenario: HOGG failure after journal commitment requires recovery
    Given a HOGG replacement has a durably committed journal frame
    When applying that frame fails
    Then the call returns recovery-required and records the cause
    And new content access is blocked until recovery completes
    When I recover the source
    Then the committed frame is replayed idempotently
    And the tree publishes the recovered state before its callbacks

  @EDIT-006
  Scenario: A read-only HOGG open reports pending recovery
    Given a HOGG has a valid committed but unapplied journal frame
    When I open it for reading without recovery permission
    Then the result is recovery-required with no usable source output

  @EDIT-007
  Scenario Outline: PIGG edits validate a clone before replacement
    Given a PIGG has colliding records and opaque unaffected metadata
    When I request a complete <edit>
    Then a clone contains the intended additions, replacements, or exclusions
    And unaffected records, payload representations, and metadata survive
    And the clone is fully validated before it replaces the original
    Examples:
      | edit        |
      | addition    |
      | replacement |
      | deletion    |

  @EDIT-008
  Scenario: A failed clone validation leaves the original PIGG unchanged
    Given a PIGG edit has constructed a clone
    When clone validation detects an invalid record or checksum
    Then the call fails before publication
    And the original archive is byte-for-byte unchanged
    And cleanup removes the unpublished clone

  @EDIT-009
  Scenario: Failure after native replacement reports commitment honestly
    Given a validated clone has replaced its native archive path
    When a subsequent durability or cleanup operation fails
    Then the call returns committed with the failure in error.cause
    And it does not claim that the old archive was preserved

  @EDIT-010
  Scenario: A concurrent external edit is not silently overwritten
    Given an import captured the destination generation
    When an uncoordinated external tool changes that destination
    And the change is detected before commitment
    Then the import fails as stale or retryable
    And the external edit remains published after reconciliation
