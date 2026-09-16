Feature: Snapshot enumeration of files and directories

  @ENTRY-001
  Scenario: List immediate children including empty directories
    Given a requested loose root has files and nested empty directories
    When I enumerate its immediate entries
    Then each direct child appears once in canonical order
    And only file entries transfer owned file selections
    And recursive enumeration includes every descendant directory

  @ENTRY-002
  Scenario: Archive paths imply directories
    Given a PIGG or HOGG contains "a/b/file"
    When I enumerate its recursive entries
    Then directories "a" and "a/b" have implied metadata
    And its file-only enumeration still contains only "a/b/file"

  @ENTRY-003
  Scenario: Entry snapshots survive source changes
    Given a cursor captures files and directories
    When the source is rescanned after a directory is removed
    Then the old cursor retains its captured entries
    And a new cursor reflects the removal

  @ENTRY-004
  Scenario: Tree directories hide same-name files
    Given one source has file "empty" and another has directory "empty"
    When I enumerate the requested tree
    Then "empty" appears as a directory
    And file-only lookup and listing do not expose the hidden file
