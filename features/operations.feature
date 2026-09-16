Feature: Caller-controlled streams and synchronous workflows

  @WORK-001
  Scenario: Opening and indexing complete synchronously
    Given an archive has many entries and long names
    When I open it as a source
    Then all archive pathname metadata is indexed before return
    And the source output stays empty if indexing fails
    And no loose subtree is recursively scanned at source open

  @WORK-002
  Scenario: The caller chooses transfer scheduling
    Given an independent reader and an unfinished writer
    When I read and write chunks of sizes I choose
    Then each call returns an exact byte count before another chunk begins
    And I can yield my coroutine between chunk calls
    And finishing the writer may block until publication completes

  @WORK-003
  Scenario: Closing unfinished staging aborts publication
    Given a builder with a partly written entry
    When I close that entry writer and then close the builder
    Then neither the entry nor the archive is published

  @WORK-004
  Scenario: Polling completes callback delivery before return
    Given many visible changes are pending in watched scopes
    When I poll synchronously
    Then all callbacks in the finite observation cut complete before return
    And no callback in that cut is delivered twice

  @WORK-005
  Scenario: A source delete reports irreversible partial progress
    Given a loose source directory contains many files
    When its explicit deletion unlinks one file and then encounters an I/O error
    Then the call returns partial
    And the error cause identifies the underlying failure
    And remaining files are not described as deleted

  @WORK-006
  Scenario: Handle ownership and failure outputs are explicit
    Given valid output storage and a context
    When a call fails before producing a handle
    Then the output handle is null
    And the diagnostic identifies the failure
    And closing a context with outstanding children returns busy

  @WORK-007
  Scenario: Independent readers permit serialized multi-thread access
    Given one control thread owns a tree
    And two independent readers refer to that tree's sources
    When workers read caller-sized chunks with serialized access per handle
    Then reader work may proceed concurrently
    And attached-source publication remains on the tree's control thread

  @WORK-008
  Scenario: Blocking helpers and stream composition agree
    Given identical inputs to a whole-file archive export and a streamed export
    When both workflows complete successfully
    Then they choose identical visible files and produce identical contents
    And they obey identical publication and ownership rules

  @WORK-009
  Scenario: Unpack target pins a captured root
    Given a cursor capturing multiple archive files and an existing directory
    When I open an unpack target
    Then all captured paths are preflighted before publication
    And substituting the root before writer open or finish fails stale

  @WORK-010
  Scenario: Close consumes an owned pointer synchronously
    Given a reader with no pending work
    When I close it through its owned pointer address
    Then the owned reader pointer is null when the call returns
    And cleanup failure is reported without restoring ownership

  @WORK-011
  Scenario: Pack and unpack request loose roots internally
    Given a loose source contains files but no subtree has been requested
    When I pack that source and unpack it into an empty native directory
    Then both operations include every currently discoverable loose file
    And neither operation requires the caller to request the root first

  @WORK-012
  Scenario: A cached miss does not hide an externally added write target
    Given a requested loose subtree has no file "menu/new"
    When an external editor creates "menu/NEW"
    And I write "menu/new" through the source API
    Then the writer replaces the current native winner "menu/NEW"
    And it does not create a second physical file "menu/new"
