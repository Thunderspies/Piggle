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

  @DISCOVERY-015
  Scenario: A shallow request permits pruning before traversal
    Given a loose root contains an empty directory and a nested file
    When I discover only its immediate children
    Then I can list its files and child directories
    And recursive listing still reports incomplete coverage
    And child directories are never opened until requested

  @DISCOVERY-011
  Scenario: Manage native paths without indexing files
    Given a tree with a loose source and an unqueried nested file
    When I manage the root recursively and enable native observation
    Then recursive listing still requires discovery
    And editing the unqueried file reports an invalidation
    When I query the file and edit it again
    Then the next report includes its before and after metadata

  @DISCOVERY-012
  Scenario: Manage known absence and directories
    Given native management of a loose root
    And a lookup has established that a managed name is absent
    When that name is created
    Then an addition is reported
    And directory changes are observable without file changes

  @DISCOVERY-013
  Scenario: Scan management establishes a baseline
    Given a managed loose scope with no prior discovery
    When I enable scan observation
    Then the scope is discovered at the registered depth
    And subsequent polls compare its visible entries

  @DISCOVERY-014
  Scenario: Restore management after a loose root disappears
    Given a managed loose root with discovered and unknown paths
    When the root is moved away
    Then polling reports loss and scope invalidation
    And it does not recursively discover unknown paths
    When a directory is created at the original root path
    Then native monitoring resumes there
    And previous selections cannot silently retarget to the new root

  @DISCOVERY-016
  Scenario: Discovery while watching includes existing directories
    Given native observation of a managed scope with an empty directory
    When I discover the scope and a file is edited
    Then only the file transition is reported
    And the existing directory does not produce an initial addition

  @DISCOVERY-017
  Scenario: Managed setup and root repair preserve failed observations
    Given cached metadata in a managed loose scope and an attached archive
    When watch setup or root replacement refresh encounters a corrupt archive
    Then the refresh fails without replacing prior cached observations

  @DISCOVERY-018
  Scenario: Callback lookup leaves new loss for the next poll
    Given a visible report is being delivered
    When callback lookup detects a root replacement
    Then its loss and scope invalidation wait for the next poll

  @DISCOVERY-019
  Scenario: Monitoring follows a renamed directory
    Given native management with a known file and a known absent destination
    When its parent directory is renamed inside the root
    Then the old name is removed and the new name is added
    And subsequent file edits are reported under the new name
