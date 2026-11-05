import Foundation

/// Turns macOS's own "Move left/right a space" and "Switch to Desktop N" shortcuts off, and
/// puts them back as they were. Opt-in: ISS's own hotkeys already win while the app runs, but
/// with the native ones off nothing falls back to the slow animated switch.
enum NativeShortcutManager {
  private static let domain = "com.apple.symbolichotkeys" as CFString
  private static let hotKeysKey = "AppleSymbolicHotKeys" as CFString
  private static let backupKey = "nativeShortcutsBackup"

  /// 79...82 are Move left/right a space; 118...133 are Switch to Desktop 1...16.
  static let shortcutIDs: [Int] = [79, 80, 81, 82] + Array(118...133)

  /// Sets "enabled" to false on each listed shortcut that exists, and returns the IDs it
  /// changed with their previous state.
  static func disabling(_ ids: [Int], in hotKeys: [String: Any]) -> (result: [String: Any], previous: [String: Bool]) {
    var result = hotKeys
    var previous: [String: Bool] = [:]
    for id in ids {
      let key = String(id)
      guard var entry = result[key] as? [String: Any] else { continue }
      let wasEnabled = (entry["enabled"] as? Bool) ?? ((entry["enabled"] as? NSNumber)?.boolValue ?? true)
      previous[key] = wasEnabled
      entry["enabled"] = false
      result[key] = entry
    }
    return (result, previous)
  }

  /// Puts each shortcut in `previous` back to its recorded state.
  static func restoring(_ previous: [String: Bool], in hotKeys: [String: Any]) -> [String: Any] {
    var result = hotKeys
    for (key, wasEnabled) in previous {
      guard var entry = result[key] as? [String: Any] else { continue }
      entry["enabled"] = wasEnabled
      result[key] = entry
    }
    return result
  }

  static var isDisabledByUs: Bool {
    UserDefaults.standard.dictionary(forKey: backupKey) != nil
  }

  @discardableResult
  static func setNativeShortcutsDisabled(_ disabled: Bool) -> Bool {
    CFPreferencesAppSynchronize(domain)
    guard let current = CFPreferencesCopyValue(
      hotKeysKey, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost) as? [String: Any]
    else { return false }

    let updated: [String: Any]
    if disabled {
      guard !isDisabledByUs else { return true }
      let change = disabling(shortcutIDs, in: current)
      UserDefaults.standard.set(change.previous, forKey: backupKey)
      updated = change.result
    } else {
      guard let backup = UserDefaults.standard.dictionary(forKey: backupKey) as? [String: Bool] else {
        return true
      }
      updated = restoring(backup, in: current)
      UserDefaults.standard.removeObject(forKey: backupKey)
    }

    CFPreferencesSetValue(
      hotKeysKey, updated as CFDictionary, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)
    CFPreferencesSynchronize(domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost)
    reloadSettings()
    return true
  }

  /// Asks the system to re-read the shortcut settings without a logout.
  private static func reloadSettings() {
    let task = Process()
    task.executableURL = URL(
      fileURLWithPath: "/System/Library/PrivateFrameworks/SystemAdministration.framework/Resources/activateSettings")
    task.arguments = ["-u"]
    try? task.run()
  }
}
