import XCTest

@testable import InstantSpaceSwitcher

final class NativeShortcutManagerTests: XCTestCase {
  private func sample() -> [String: Any] {
    [
      "79": ["enabled": true, "value": ["type": "standard"]],
      "118": ["enabled": false],
      "999": ["enabled": true],
    ]
  }

  /// Only known desktop shortcuts are disabled, and their previous state is recorded.
  func testDisablingRecordsPreviousStateAndLeavesOthersAlone() {
    let change = NativeShortcutManager.disabling(NativeShortcutManager.shortcutIDs, in: sample())

    XCTAssertEqual(change.previous, ["79": true, "118": false])
    XCTAssertEqual((change.result["79"] as? [String: Any])?["enabled"] as? Bool, false)
    XCTAssertNotNil((change.result["79"] as? [String: Any])?["value"])
    XCTAssertEqual((change.result["999"] as? [String: Any])?["enabled"] as? Bool, true)
  }

  /// Restoring returns each shortcut to what it was, including ones that were already off.
  func testRestoringBringsBackThePreviousState() {
    let original = sample()
    let change = NativeShortcutManager.disabling(NativeShortcutManager.shortcutIDs, in: original)
    let restored = NativeShortcutManager.restoring(change.previous, in: change.result)

    XCTAssertEqual((restored["79"] as? [String: Any])?["enabled"] as? Bool, true)
    XCTAssertEqual((restored["118"] as? [String: Any])?["enabled"] as? Bool, false)
  }
}
