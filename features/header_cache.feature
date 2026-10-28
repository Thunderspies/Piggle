Feature: Cache resource headers independently and inside archives

  @CACHE-001
  Scenario Outline: Archive writers cache supported resource headers
    Given a "<format>" builder for a resource with a valid header
    When I stage the resource with "automatic" header caching
    Then its archive metadata contains the selected header
    And logical prefix reads use that cached header
    And a payload-only reader returns the original resource

    Examples:
      | format |
      | PIGG   |
      | HOGG   |

  @CACHE-002
  Scenario Outline: Callers control archive header caching
    Given a "HOGG" builder for a resource with a valid header
    When I stage the resource with "<policy>" header caching
    Then its archive metadata contains the selected header
    And logical prefix reads use that cached header
    And a payload-only reader returns the original resource

    Examples:
      | policy     |
      | suppressed |
      | explicit   |

  @CACHE-003
  Scenario: Resource header inference does not need an archive
    Given logical resource bytes with a length-prefixed header
    When I classify and extract its header independently
    Then the independent cache contains its complete header prefix

  @CACHE-004
  Scenario Outline: Standalone containers retain binary slots and holes
    Given independent binary cache slots with empty entries
    When I encode and decode the slots as a "<container>"
    Then every slot retains its index and original bytes

    Examples:
      | container     |
      | PIGG pool     |
      | HOGG DataList |
