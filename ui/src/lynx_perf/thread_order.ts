/**
 * Thread ordering priority values for track visualization.
 *
 * Note: Smaller sortOrder values indicate higher priority in the display hierarchy.
 *
 * Default priority values:
 * - Main thread: -25
 * - Lynx JS thread: -20
 * - Other Lynx threads: -15
 * - Default/fallback: 20
 */
export enum ThreadSortOrder {
  // Highest priority performance issues track
  PERFORMANCE_ISSUES = -1000,

  // Vital timestamp markers
  VITAL_TIMESTAMP = -30,

  // Scroll performance tracking
  SCOLL = -26,

  // Main UI thread
  MAIN_THREAD = -25,

  // Lynx Background thread
  LYNX_BACKGROUND_THREAD = -20,

  // Other Lynx-specific threads
  LYNX_THREAD = -15,

  // Default priority for all other threads
  OTHER_THREAD = 20,
}
