Feature: Whole-file sequential transfer and publication

  @IO-001
  Scenario: Import targets an explicit source and stays private until complete
    Given "base" contains "a" and loses to "patch"
    When I import a native file into "a" in destination source "base"
    And only part of the input has been transferred
    Then the old base copy and current patch winner remain published
    When validation and commitment complete
    Then only the base copy is replaced
    And the patch copy remains visible

  @IO-002
  Scenario: Arbitrary input and output chunk boundaries preserve bytes
    Given a binary file containing zero bytes and 257 logical bytes
    When I write it in chunks of 1, 7, 64, and the remaining bytes
    And I finish and read it with output capacities of 3 and 11 repeatedly
    Then the concatenated output equals all 257 input bytes
    And each transfer count advances only by bytes consumed or produced

  @IO-003
  Scenario: Empty files are complete replacements
    Given the destination contains "old"
    When I finish a replacement without writing any bytes
    Then the destination is an existing empty regular file
    And its reader reaches end with zero bytes

  @IO-004
  Scenario: Export replaces an existing regular native file
    Given the selected file contains "new"
    And the native destination is a regular file containing "old"
    When I export the selected file with overwrite enabled and complete commitment
    Then the native destination contains exactly "new"

  @IO-005
  Scenario Outline: Ordinary replacement rejects non-regular conflicts
    Given the native replacement target is a <kind>
    When I import, export, or pack a regular file at that target
    Then the result is a type-conflict error
    And the conflicting object and anything it refers to are unchanged
    Examples:
      | kind             |
      | directory        |
      | symbolic link    |
      | Windows junction |
      | special file     |

  @IO-006
  Scenario: Closing staged content preserves the old destination
    Given a destination contains "old"
    And a replacement has staged "new" without commitment
    When I close the unfinished writer
    Then the close succeeds
    And the destination still contains "old"
    And no staged copy is visible

  @IO-007
  Scenario: Closing an unfinished write aborts it
    Given a writer has accepted bytes but has not finished
    When I close it and complete cleanup
    Then the old destination is preserved
    And no replacement is committed

  @IO-008
  Scenario: Corrupt compression is rejected before import commitment
    Given stored input declares compressed logical contents
    And its compressed stream is malformed or truncated
    When I import it and finish validation
    Then the result is a corrupt-data error
    And the old destination is preserved

  @IO-009
  Scenario: A checksum mismatch prevents publishing an import or export
    Given compressed input decodes but its expected digest is wrong
    When I import or export it and complete validation
    Then the result is a checksum error
    And the old destination is preserved

  @IO-010
  Scenario: Streaming reads can fail after delivering a prefix
    Given a selected archive file has a bad stored checksum
    When I sequentially read the file
    Then a prefix may be delivered before the checksum error
    And reported byte counts describe exactly the delivered prefix
    And successful bytes do not imply successful final verification

  @IO-011
  Scenario: Whole-file transfers expose no random modification mode
    Given an open sequential transfer
    Then the public interface provides no seek, positional I/O, or append
    And a write always describes a complete replacement

  @IO-012
  Scenario: Pack and unpack use the same resolution and replacement rules
    Given a tree has visible files "a" and "dir/b" and losing copies
    When I pack its visible view and unpack with overwrite enabled
    Then the output contains the visible contents at "a" and "dir/b"
    And losing copies and empty directories are not exported
    And each unpacked regular file replaces any existing regular file

  @IO-013
  Scenario: A later unpack failure preserves files already committed
    Given unpack has committed "a" and is staging "b"
    When processing "b" fails before publication
    Then "a" remains replaced
    And the previous "b" remains unchanged
    And the call returns partial with the underlying cause

  @IO-014
  Scenario: Allocated reads are bounded verified binary results
    Given a selected logical file has three bytes including a zero byte
    When I request an allocated read with a two-byte limit
    Then the result is a limit error with an empty buffer
    When I read with a three-byte limit
    Then the buffer is published only after verification and cleanup
    And its three bytes remain owned after context close
    And freeing the buffer clears its pointer and length

  @IO-015
  Scenario: Build an archive with one publication boundary
    Given an archive builder has staged several entries
    When an entry writer finishes successfully
    Then no external archive has been published
    And a live entry writer prevents builder finish or close
    When I close the entry writer and finish the builder
    Then the complete archive is published once
    And an accepted builder finish makes the builder close-only

  @IO-016
  Scenario: Native output requires explicit overwrite
    Given a regular native output already exists
    When I export or create a native writer without overwrite
    Then the result is an exists error and the original is unchanged
    When I explicitly enable overwrite and finish successfully
    Then the regular output is replaced
    And links and special objects remain conflicts

  @IO-017
  Scenario: An archive builder rejects duplicate canonical names
    Given an archive builder has staged an entry named "Dir/A"
    When I try to stage another entry named "dir/a"
    Then the second entry returns an exists error
    And the first staged entry remains available for builder finish
