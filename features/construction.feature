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

