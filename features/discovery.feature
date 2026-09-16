Feature: Explicit loose discovery and indexed enumeration

  @DISCOVERY-001
  Scenario: Opening a loose source does not scan descendants
    Given a loose root contains a readable file "a/b" and an unreadable "z"
    When I open that root as a source
    Then opening succeeds without visiting "z"
    When I find "a/b"
    Then only the requested path and its ancestors are probed

  @DISCOVERY-002
  Scenario: A subtree request retains its discovered names
    Given an ordered tree has a loose source containing "menu/a"
    When I request subtree "menu"
    Then its loose names are cached before the call returns
    And I capture a cursor for "menu"
    When an editor adds "menu/b" while watching is off
    Then another cursor still yields only "menu/a"
    When I request "menu" again and capture a new cursor
    Then the new cursor yields "menu/a, menu/b"
    And an earlier cursor retains its captured order and metadata

  @DISCOVERY-003
  Scenario: Archive pathname metadata is indexed at source open
    Given an archive has many file records
    When I open the archive as a source
    Then pathname indexing completes before the call returns
    And invalid archive pathname metadata fails that open

  @DISCOVERY-004
  Scenario: A loose listing requires complete requested coverage
    Given a loose source contains "menu/a" and "other/b"
    When I enumerate "menu" before requesting it
    Then listing reports an invalid state and no cursor
    When I request "menu" on the source
    Then listing "menu" returns a complete captured cursor
    And listing the source root still reports an invalid state

  @DISCOVERY-005
  Scenario: A parent request covers child listings and indexed misses
    Given a loose source contains "menu/a" and watching is off
    When I request the root and then an editor adds "menu/b"
    Then listing "menu" yields only "menu/a" without rescanning
    And finding "menu/b" reports not found from the index
    When I request "menu" again
    Then listing "menu" yields "menu/a, menu/b"
    And finding "menu/b" succeeds

  @DISCOVERY-006
  Scenario: Unrequested exact lookup still probes the loose path
    Given a loose source has not requested "menu"
    When an editor adds "menu/new" and I find "menu/new"
    Then exact lookup selects the current loose copy
    And listing "menu" still reports an invalid state

  @DISCOVERY-007
  Scenario: Archive-only enumeration needs no subtree request
    Given an archive source contains "menu/a"
    When I enumerate "menu" without requesting it
    Then a complete cursor containing "menu/a" is returned

  @DISCOVERY-008
  Scenario: Hybrid listing requires a tree request
    Given a tree has an archive and a loose source under "menu"
    When I enumerate "menu" before a tree request
    Then listing reports an invalid state and no partial archive cursor
    When I request "menu" on the tree
    Then enumeration returns the complete overlay in canonical order

  @DISCOVERY-009
  Scenario: Large recursive discovery remains synchronous
    Given a loose subtree contains many files
    When I request that subtree
    Then its index is complete when the call returns
    And a later listing needs no operation handle or directory scan

  @DISCOVERY-010
  Scenario: Cached selection does not retarget after external deletion
    Given a requested loose subtree contains "menu/a" while watching is off
    When an editor removes "menu/a" and I find it again
    Then find returns the cached selection and metadata
    And opening that physical copy reports stale
