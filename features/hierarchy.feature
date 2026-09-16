Feature: Directories win over same-name files

  @TREE-001
  Scenario: A directory remains visible beneath a later attached file
    Given source "base" contains "a/b" and "a/c/d"
    And source "patch" containing "a" is attached later
    When I request the root and enumerate visible files
    Then "a/b" and "a/c/d" are returned
    And resolving "a" reports a directory conflict

  @TREE-002
  Scenario: An implied directory hides an earlier file
    Given source "base" contains "a"
    And source "patch" contains "a/b"
    When I resolve "a"
    Then the result is a directory conflict
    When I request the root and enumerate visible files
    Then only "a/b" is returned

  @TREE-003
  Scenario: Descendants from multiple sources share one directory
    Given source "low" contains "a/b"
    And source "middle" contains "a"
    And source "high" contains "a/c"
    When I request the root and enumerate visible files
    Then the names are "a/b, a/c"
    And "a" is a directory conflict

  @TREE-004
  Scenario: A same-source directory wins over an exact file
    Given one existing archive contains "a" and "a/b"
    When I request the root and enumerate visible files
    Then only "a/b" is returned
    And resolving "a" reports a directory conflict

  @TREE-005
  Scenario: Detaching a subtree reveals an earlier file
    Given source "file" contains "a"
    And source "subtree" contains "a/b" and "a/c"
    And the subtree "a" has been requested while watching
    When I detach "subtree" and poll
    Then a new lookup selects the file "a"
    And a visible callback reports removal of the descendants
    And a visible callback reports addition of "a"
