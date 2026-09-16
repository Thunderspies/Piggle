Feature: Ordered sources and visible file selection

  @RANK-001
  Scenario: Later attachment wins regardless of timestamps or source kind
    Given an archive and a loose source both contain "a"
    When I attach the archive and then the loose source
    Then the selected source for "a" is the loose source
    And changing timestamps does not change that priority

  @RANK-002
  Scenario: Reattachment moves a source to the end
    Given "base" and "patch" both contain "a"
    And "patch" was attached after "base"
    When I detach and reattach "base"
    Then a new lookup selects "base"

  @RANK-003
  Scenario: Ordered open gives later entries priority
    Given "base", "patch", and "loose" each contain "a"
    When I open a tree from the ordered list "base, patch, loose"
    Then the selected source for "a" is "loose"

  @RANK-004
  Scenario: Held selections do not follow attachment changes
    Given "base" loses "a" to "patch"
    And I hold the selected "patch" file
    When I detach and reattach "base"
    Then a new lookup selects "base"
    And the held file still identifies the "patch" copy

  @RANK-005
  Scenario: Later archive records win within one source
    Given an existing archive contains "A" as record 1 and "a" as record 2
    When I resolve "a" in that source
    Then the selected contents are from record 2
    And no API cursor exposes record 1 as a losing copy

  @RANK-006
  Scenario: Loose collisions use original path bytes
    Given a case-sensitive loose source contains "Dir/A" and "dir/a"
    When I resolve "dir/a"
    Then the original relative path is "dir/a"
    When I request the loose root and enumerate it
    Then enumeration returns one visible file for that name

  @RANK-007
  Scenario: Only visible copies are selectable
    Given two attached sources and an existing archive contain colliding "a"
    When I request the root, enumerate visible files and resolve "a"
    Then enumeration returns one "a" and lookup selects its winner
    And physical validation still checks the losing archive record
