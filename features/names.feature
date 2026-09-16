Feature: Virtual names and explicit native sources
  Canonical names are byte strings. Native paths are a separate domain.

  @NAME-001
  Scenario: Normalize separators and ASCII letters without Unicode folding
    When I normalize the name "Textures\ÉTÉ.DDS"
    Then the canonical name is "textures/ÉtÉ.dds"
    And the non-ASCII bytes have not changed

  @NAME-002
  Scenario: Both lookup layers normalize virtual names
    Given a visible file named "textures/a.dds"
    When I find "Textures\A.DDS" through the source API
    Then I select the copy visible as "textures/a.dds"
    When I open "Textures\A.DDS" as a named reader
    Then I select the copy visible as "textures/a.dds"

  @NAME-003
  Scenario Outline: Reject names that escape the canonical domain
    When I normalize "<name>"
    Then the result is an invalid-name error
    And no destination is changed
    Examples:
      | name        |
      | /root/file  |
      | C:/file     |
      | a/../file   |
      | a:b         |
      | \\root      |
      |             |

  @NAME-004
  Scenario: Normalize harmless redundant path components in every lookup
    When I normalize "A//./B/"
    Then the canonical name is "a/b"
    And synchronous lookup accepts the equivalent spelling "a//b"

  @NAME-005
  Scenario: Keep native paths separate from virtual names
    Given the source has native path "C:\Game\Data.PIGG"
    And it contains the canonical name "textures/a.dds"
    When I find "Textures\A.DDS" through that source handle
    Then I select that source's copy of "textures/a.dds"
    And the native source path remains "C:\Game\Data.PIGG"
    When I use "C:\Game\Data.PIGG:textures/a.dds" as a virtual name
    Then the result is an invalid-name error

  @NAME-006
  Scenario: Open and query standalone sources explicitly
    Given "Extra.PIGG" exists but is not attached
    When I explicitly open "Extra.PIGG" as a source
    Then I can find "data/a" through that source handle
    And the source is still not attached to the tree
