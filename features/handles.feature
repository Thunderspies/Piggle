Feature: Stable copy identity and metadata iteration

  @HANDLE-001
  Scenario: A selected handle never switches to a new winner
    Given I selected the base copy of "a"
    When a stronger source containing "a" is attached
    Then the held handle still selects the base copy
    And opening a read on it reads the base copy if unchanged

  @HANDLE-002
  Scenario: Replacing the selected physical copy makes its handle stale
    Given I selected a copy of "a" with contents "old"
    When an external tool replaces that copy with contents "new"
    And reconciliation completes
    Then the old handle retains its original metadata
    And a new read on that handle returns a stale-copy error
    And it does not read "new" through a substituted handle

  @HANDLE-003
  Scenario: Tree iteration keeps its captured lexical metadata view
    Given a cursor captures visible names "a, c"
    When "a" is removed and "b" is added and reconciliation completes
    Then that cursor yields "a" and "c" one entry at a time
    And a new cursor yields "b" and "c" one entry at a time
    And reading the old "a" may return a stale-copy error
    And exhaustion reports end-of-iteration with no file output

  @HANDLE-004
  Scenario: Visible iteration keeps captured metadata during updates
    Given a cursor captures visible files "a, c"
    When "a" is replaced and "b" is added
    Then the cursor still returns "a, c" with captured metadata
    And a new cursor returns "a, b, c" with current metadata

  @HANDLE-005
  Scenario: Two transfers have independent sequential positions
    Given two read transfers refer to the same selected file "abcd"
    When the first consumes 3 bytes and the second consumes 1 byte
    Then the first next returns "d"
    And the second next returns "bcd"

  @HANDLE-006
  Scenario: Detachment and closing preserve source data
    Given a file and cursor retain a source attached to a tree
    When I detach that source and close my source reference
    Then the source data still exists
    And the retained file and cursor keep their metadata
    And unchanged retained files can still be read
    And tree lookup no longer selects that detached source

  @HANDLE-007
  Scenario: Workers traverse a tree while control updates its view
    Given workers share a live tree with its control thread
    When workers look up files and capture independent file and entry listings
    And the control thread attaches, detaches or refreshes sources
    Then each traversal captures a complete synchronized view
    And captured cursors retain lexical order and original metadata
    And workers can advance and close their own cursors and selected files
    And named reads never switch selected copies on stale content

  @HANDLE-008
  Scenario: Callback work can traverse the tree on another thread
    Given a watched tree has queued visible changes
    When its observer starts and waits for a worker lookup and listing
    Then the worker completes without an internal lock blocking the callback
    And reports queued during the callback wait for the next poll
    And mutation and recursive poll remain restricted
