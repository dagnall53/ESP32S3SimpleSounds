// Adds the filé systems
#pragma once
#include <Arduino.h>  // defines `byte`, `boolean`, `String`, etc.
#include <FS.h>       // defines fs
#include <ESPFMfGK.h>

extern const word filemanagerport; // in case anything elsewhere wants to reference/use this value
extern ESPFMfGK filemgr;
void addFileSystems(void); 
uint32_t checkFileFlags(fs::FS &fs, String filename, uint32_t flags);
void setupFilemanager(void) ;
//