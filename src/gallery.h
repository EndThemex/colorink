#ifndef INKSIGHT_GALLERY_H
#define INKSIGHT_GALLERY_H

#include <Arduino.h>

// ── LittleFS image gallery ──────────────────────────────────
// Images are stored as raw 2bpp (BWRY) framebuffers, one file per image:
//   /img/<id>_<name>.raw      (COLOR_BUF_LEN bytes)
// The id of the currently displayed image is persisted in NVS.

// Initialize LittleFS. Call once at boot. Returns false on failure.
bool galleryInit();

// Total bytes and free bytes of the filesystem.
uint32_t galleryFreeBytes();

// Number of stored images.
int galleryCount();

// Build JSON: {"images":[{"id":1,"name":"cat","size":105984}],"current":1,"free":123456}
String galleryListJson();

// ── Streaming upload (multipart file handler) ───────────────
// Begin a new upload. Returns false (and sets *errOut) if storage is full.
bool galleryUploadStart(int &id, const String &name, String *errOut);
// Write the next chunk of raw framebuffer data.
void galleryUploadData(const uint8_t *buf, size_t len);
// Finish the upload. Verifies the total size; on failure the file is removed.
// Returns the final id, or -1 on failure (errOut set).
int  galleryUploadEnd(const String &name, String *errOut);

// Load image by id into colorBuf and refresh the panel (2bpp).
// Returns false if the image does not exist.
bool galleryDisplayById(int id);

// Display the next (dir=1) or previous (dir=-1) image in id order.
bool galleryCycle(int dir);

// Remove an image. If it was the current one, the current id is cleared.
bool galleryDeleteById(int id);

// Clear the panel to solid white (does not touch stored images).
bool galleryClearScreen();

// Id of the currently displayed image (-1 = none).
int galleryCurrentId();

// True if an image with this id exists.
bool galleryExists(int id);

#endif // INKSIGHT_GALLERY_H
