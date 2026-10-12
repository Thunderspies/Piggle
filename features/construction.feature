Feature: Build and maintain archives with explicit publication

  @BUILD-002
  Scenario: A large cached header preserves an unrelated reader
    Given a HOGG has an unchanged reader and an editable entry
    When I install a 64 KiB cached header on the editable entry
    Then the unchanged reader still returns its original bytes
    And the edited entry retains its payload and the complete cached header

