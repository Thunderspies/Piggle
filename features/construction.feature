Feature: Build and maintain archives with explicit publication

  @BUILD-001
  Scenario: Reserved capacity permits later archive growth
    Given a HOGG was created with room for 31 user entries
    When I add 40 named files to the existing archive
    Then every addition is readable after reopening the archive

  @BUILD-002
  Scenario: A large cached header preserves an unrelated reader
    Given a HOGG has an unchanged reader and an editable entry
    When I install a 64 KiB cached header on the editable entry
    Then the unchanged reader still returns its original bytes
    And the edited entry retains its payload and the complete cached header

  @BUILD-003
  Scenario: Compaction preserves the source and its visible content
    Given a HOGG contains obsolete payload bytes after replacement
    When I inspect its storage and compact it to another archive
    Then the compacted archive matches the visible source content
    And the original archive remains byte-for-byte unchanged

  @BUILD-004
  Scenario: Resume only committed construction progress
    Given a persistent HOGG builder uses a chosen staging directory
    When I checkpoint one entry and close after adding another entry
    And I resume the construction session
    Then its inventory contains only the checkpointed entry
    And I can add another entry and explicitly publish the archive

  @BUILD-005
  Scenario: Suspend retains all completed entries
    Given a persistent HOGG builder uses a chosen staging directory
    When I add two entries and suspend construction
    And I resume the construction session
    Then its inventory retains both entries in insertion order
    And I can discard construction without publishing an archive

  @BUILD-006
  Scenario: A copied checkpoint cannot acquire a live payload
    Given a persistent HOGG builder uses a chosen staging directory
    When I copy its checkpoint into another session directory
    Then resuming the copy reports busy without acquiring a builder
    And the original builder can publish its checkpointed content
