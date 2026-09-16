Feature: Caller-polled visible changes in requested scopes

  @CHANGE-001
  Scenario: An open tree reader tracks its file
    Given native watching is active
    When I open a tree reader for "a"
    And an editor replaces "a" before the reader closes
    And I poll
    Then one visible update for "a" is delivered

  @CHANGE-002
  Scenario: Unopened files do not create watch scopes
    Given native watching is active
    And no subtree has been requested
    When an editor changes an unopened file "b"
    And I poll
    Then no visible event for "b" is delivered
    And a later exact lookup probes the current loose file

  @CHANGE-003
  Scenario: Closing a reader ends its file watch
    Given native watching is active
    And a tree reader for "a" has been opened
    When I close the reader and then an editor changes "a"
    And I poll
    Then no new event for "a" is delivered
    And reports queued before close remain deliverable

  @CHANGE-004
  Scenario: A requested subtree stays watched
    Given native watching is active
    When I request subtree "menu"
    And an editor adds "menu/new"
    And I poll
    Then a visible addition for "menu/new" is delivered

  @CHANGE-005
  Scenario: Source changes report only affected visible names
    Given a watched subtree contains "a" and "b"
    When I attach and then detach a source that replaces only "a"
    And I poll after each change
    Then visible winner changes for "a" are delivered
    And no event for unchanged "b" is delivered

  @CHANGE-006
  Scenario: Poll callbacks see reconciled state
    Given a visible update for watched "a" is queued
    When I poll on the tree control thread
    Then callback lookup sees the updated "a"
    And recursive polling reports a reentrancy error

  @CHANGE-007
  Scenario: Native echoes do not duplicate library changes
    Given an open reader tracks "a" while native watching is active
    When Piggle replaces "a" and its native echo arrives
    And I poll
    Then exactly one visible update for "a" is delivered

  @CHANGE-008
  Scenario: External intermediate edits may coalesce
    Given "a" is watched
    When an editor completes three replacements before one poll
    Then the poll may report one transition to the latest stable state
    And it does not promise revision history

  @CHANGE-009
  Scenario: Notification loss invalidates watched scopes
    Given native watching is active for subtree "menu"
    When the native notification queue loses events
    And I poll synchronously
    Then a loss event identifies "menu" when provenance is known
    And reconciliation refreshes only tracked scopes

  @CHANGE-010
  Scenario: Explicit rescan refreshes requested scopes
    Given an external edit was missed in requested subtree "menu"
    When I synchronously rescan and then poll
    Then the cached subtree reflects the stable edit
    And its visible change is delivered if watching is active

  @CHANGE-011
  Scenario: Watch and unwatch are synchronous session boundaries
    Given a new tree starts with watching off
    Then polling reports invalid state
    When I enable scan watching
    Then active scopes gain baselines without initial reports
    And enabling the same mode preserves queued changes
    When I unwatch
    Then monitoring stops and queued reports are discarded before return

  @CHANGE-012
  Scenario: Refresh without watching keeps snapshots stable
    Given watching is off and a cursor captured names "a, c"
    When an editor removes "a" and adds "b"
    Then another cursor still yields "a, c"
    When I request the same subtree again
    Then a new cursor yields "b, c"
    And the old cursor still yields "a, c"

  @CHANGE-013
  Scenario: Native hints update requested indexes before queries return
    Given native watching is active for requested subtree "menu"
    When an editor adds "menu/new"
    And I list "menu" before polling
    Then the cursor contains "menu/new" without a full quiet-state rescan
    And a named read of "menu/new" succeeds
    And the visible addition remains queued for a later poll

  @CHANGE-014
  Scenario: Scan watching refreshes on poll rather than listing
    Given scan watching is active for requested subtree "menu"
    When an editor adds "menu/new"
    Then listing "menu" still uses the previous index
    When I poll and list "menu" again
    Then the cursor contains "menu/new"

  @CHANGE-015
  Scenario: A source-local request does not create a tree watch scope
    Given native watching is active on a tree with one loose source
    When I request "menu" only on the attached source
    And an editor adds "menu/new" and I poll
    Then no visible event for "menu/new" is delivered
    And source-local listing still uses its previous index
    When I request "menu" on the tree
    Then tree listing contains "menu/new"
