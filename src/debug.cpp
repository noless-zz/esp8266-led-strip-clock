#include "debug.h"
#include "globals.h"

void debugLogf(const char* level, const char* tag, const char* fmt, ...) {
  char msg[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.printf("[%lu][%s][%s] %s\n", millis(), level, tag, msg);
  if (!debugRemoteEnabled || !wifiConnected || debugServerIp.length() == 0) return;
  char packet[320];
  int plen = snprintf(packet, sizeof(packet), "[%lu][%s][%s] %s\n", millis(), level, tag, msg);
  debugUdp.beginPacket(debugServerIp.c_str(), debugServerPort);
  debugUdp.write((const uint8_t*)packet, (size_t)plen);
  debugUdp.endPacket();
}

void captureBootInfo() {
  rst_info *ri = ESP.getResetInfoPtr();

  // Read RTC boot record (valid across WDT/exception resets, not power cycles)
  BootRecord br = {};
  bool rtcValid = ESP.rtcUserMemoryRead(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br))
                  && (br.magic == RTC_BOOT_MAGIC);
  uint32_t lastUptime_s  = rtcValid ? br.uptime_s  : 0;
  uint32_t bootCount     = rtcValid ? br.boot_count + 1 : 1;

  // Update record immediately so loop() can increment uptime from zero
  br.magic      = RTC_BOOT_MAGIC;
  br.uptime_s   = 0;
  br.boot_count = bootCount;
  ESP.rtcUserMemoryWrite(RTC_BOOT_SLOT, (uint32_t*)&br, sizeof(br));

  // Format last uptime as h:mm:ss
  char uptimeBuf[24];
  if (rtcValid) {
    snprintf(uptimeBuf, sizeof(uptimeBuf), "%uh%02um%02us",
             lastUptime_s / 3600, (lastUptime_s % 3600) / 60, lastUptime_s % 60);
  } else {
    snprintf(uptimeBuf, sizeof(uptimeBuf), "unknown (power-on)");
  }

  // Build reset-reason string
  char reason[128];
  snprintf(reason, sizeof(reason), "reason=%d (%s)", ri->reason, ESP.getResetReason().c_str());
  // Crash reasons: 1=HW WDT, 2=Exception, 3=Soft WDT  (4=ESP.restart() is intentional)
  bool isCrash = (ri->reason == 1 || ri->reason == 2 || ri->reason == 3);
  if (isCrash) {
    char crash[128];
    snprintf(crash, sizeof(crash), " CRASH! exccause=%d epc1=0x%08X excvaddr=0x%08X",
             ri->exccause, ri->epc1, ri->excvaddr);
    cachedBootInfo = String(reason) + String(crash);
  } else {
    cachedBootInfo = String(reason);
  }

  Serial.printf("[BOOT] #%u  last_uptime=%s  %s\n",
                bootCount, uptimeBuf, cachedBootInfo.c_str());
}
