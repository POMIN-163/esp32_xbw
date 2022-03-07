/*
  Created by Fabrizio Di Vittorio (fdivitto2013@gmail.com) - <http://www.fabgl.com>
  Copyright (c) 2019-2021 Fabrizio Di Vittorio.
  All rights reserved.


* Please contact fdivitto2013@gmail.com if you need a commercial license.


* This library and related software is available under GPL v3.

  FabGL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  FabGL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with FabGL.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>

#include "ff.h"
#include "diskio.h"
#include "esp_vfs_fat.h"
#include "esp_task_wdt.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_spiffs.h"
#include "soc/efuse_reg.h"
#include "soc/rtc.h"
#include "esp_ipc.h"
#include "soc/adc_channel.h"

#include "fabutils.h"
#include "dispdrivers/vgacontroller.h"
#include "dispdrivers/vga2controller.h"
#include "dispdrivers/vga16controller.h"
#include "comdrivers/ps2controller.h"


#pragma GCC optimize ("O2")



namespace fabgl {



////////////////////////////////////////////////////////////////////////////////////////////
// TimeOut


TimeOut::TimeOut()
  : m_start(esp_timer_get_time())
{
}


bool TimeOut::expired(int valueMS)
{
  return valueMS > -1 && ((esp_timer_get_time() - m_start) / 1000) > valueMS;
}



////////////////////////////////////////////////////////////////////////////////////////////
// isqrt

// Integer square root by Halleck's method, with Legalize's speedup
int isqrt (int x)
{
  if (x < 1)
    return 0;
  int squaredbit = 0x40000000;
  int remainder = x;
  int root = 0;
  while (squaredbit > 0) {
    if (remainder >= (squaredbit | root)) {
      remainder -= (squaredbit | root);
      root >>= 1;
      root |= squaredbit;
    } else
      root >>= 1;
    squaredbit >>= 2;
  }
  return root;
}



////////////////////////////////////////////////////////////////////////////////////////////
// calcParity

bool calcParity(uint8_t v)
{
  v ^= v >> 4;
  v &= 0xf;
  return (0x6996 >> v) & 1;
}


////////////////////////////////////////////////////////////////////////////////////////////
// realloc32
// free32

// size must be a multiple of uint32_t (32 bit)
void * realloc32(void * ptr, size_t size)
{
  uint32_t * newBuffer = (uint32_t*) heap_caps_malloc(size, MALLOC_CAP_32BIT);
  if (ptr) {
    moveItems(newBuffer, (uint32_t*)ptr, size / sizeof(uint32_t));
    heap_caps_free(ptr);
  }
  return newBuffer;
}


void free32(void * ptr)
{
  heap_caps_free(ptr);
}



////////////////////////////////////////////////////////////////////////////////////////////
// msToTicks

uint32_t msToTicks(int ms)
{
  return ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}


////////////////////////////////////////////////////////////////////////////////////////////
// getChipPackage

ChipPackage getChipPackage()
{
  // read CHIP_VER_PKG (block0, byte 3, 105th bit % 32 = 9, 3 bits)
  uint32_t ver_pkg = (REG_READ(EFUSE_BLK0_RDATA3_REG) >> 9) & 7;
  switch (ver_pkg) {
    case 0:
      return ChipPackage::ESP32D0WDQ6;  // WROOOM-32
    case 1:
      return ChipPackage::ESP32D0WDQ5;  // WROVER-B
    case 2:
      return ChipPackage::ESP32D2WDQ5;
    case 5:
      return ChipPackage::ESP32PICOD4;  // TTGO-VGA32
    default:
      return ChipPackage::Unknown;
  }
}


////////////////////////////////////////////////////////////////////////////////////////////
adc1_channel_t ADC1_GPIO2Channel(gpio_num_t gpio)
{
  switch (gpio) {
    case ADC1_CHANNEL_0_GPIO_NUM:
      return ADC1_CHANNEL_0;
    case ADC1_CHANNEL_1_GPIO_NUM:
      return ADC1_CHANNEL_1;
    case ADC1_CHANNEL_2_GPIO_NUM:
      return ADC1_CHANNEL_2;
    case ADC1_CHANNEL_3_GPIO_NUM:
      return ADC1_CHANNEL_3;
    case ADC1_CHANNEL_4_GPIO_NUM:
      return ADC1_CHANNEL_4;
    case ADC1_CHANNEL_5_GPIO_NUM:
      return ADC1_CHANNEL_5;
    case ADC1_CHANNEL_6_GPIO_NUM:
      return ADC1_CHANNEL_6;
    case ADC1_CHANNEL_7_GPIO_NUM:
      return ADC1_CHANNEL_7;
    default:
      return ADC1_CHANNEL_0;
  }
}


////////////////////////////////////////////////////////////////////////////////////////////
// configureGPIO
void configureGPIO(gpio_num_t gpio, gpio_mode_t mode)
{
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio], PIN_FUNC_GPIO);
  gpio_set_direction(gpio, mode);
}


////////////////////////////////////////////////////////////////////////////////////////////
// getApbFrequency
uint32_t getApbFrequency()
{
  rtc_cpu_freq_config_t conf;
  rtc_clk_cpu_freq_get_config(&conf);
  return conf.freq_mhz >= 80 ? 80000000 : (conf.source_freq_mhz * 80000000 / conf.div);
}


////////////////////////////////////////////////////////////////////////////////////////////
// getCPUFrequencyMHz
uint32_t getCPUFrequencyMHz()
{
  rtc_cpu_freq_config_t conf;
  rtc_clk_cpu_freq_get_config(&conf);
  return conf.freq_mhz;
}


////////////////////////////////////////////////////////////////////////////////////////////
// esp_intr_alloc_pinnedToCore

struct esp_intr_alloc_args {
  int             source;
  int             flags;
  intr_handler_t  handler;
  void *          arg;
  intr_handle_t * ret_handle;
  TaskHandle_t    waitingTask;
};


void esp_intr_alloc_pinnedToCore_call(void * arg)
{
  auto args = (esp_intr_alloc_args*) arg;
  esp_intr_alloc(args->source, args->flags, args->handler, args->arg, args->ret_handle);
}


void esp_intr_alloc_pinnedToCore(int source, int flags, intr_handler_t handler, void * arg, intr_handle_t * ret_handle, int core)
{
  esp_intr_alloc_args args = { source, flags, handler, arg, ret_handle, xTaskGetCurrentTaskHandle() };
  esp_ipc_call_blocking(core, esp_intr_alloc_pinnedToCore_call, &args);
}



////////////////////////////////////////////////////////////////////////////////////////////
// converts '\' or '/' to newSep

void replacePathSep(char * path, char newSep)
{
  for (; *path; ++path)
    if (*path == '\\' || *path == '/')
      *path = newSep;
}



////////////////////////////////////////////////////////////////////////////////////////////
// Sutherland-Cohen line clipping algorithm

static int clipLine_code(int x, int y, Rect const & clipRect)
{
  int code = 0;
  if (x < clipRect.X1)
    code = 1;
  else if (x > clipRect.X2)
    code = 2;
  if (y < clipRect.Y1)
    code |= 4;
  else if (y > clipRect.Y2)
    code |= 8;
  return code;
}

// false = line is out of clipping rect
// true = line intersects or is inside the clipping rect (x1, y1, x2, y2 are changed if checkOnly=false)
bool clipLine(int & x1, int & y1, int & x2, int & y2, Rect const & clipRect, bool checkOnly)
{
  int newX1 = x1;
  int newY1 = y1;
  int newX2 = x2;
  int newY2 = y2;
  int topLeftCode     = clipLine_code(newX1, newY1, clipRect);
  int bottomRightCode = clipLine_code(newX2, newY2, clipRect);
  while (true) {
    if ((topLeftCode == 0) && (bottomRightCode == 0)) {
      if (!checkOnly) {
        x1 = newX1;
        y1 = newY1;
        x2 = newX2;
        y2 = newY2;
      }
      return true;
    } else if (topLeftCode & bottomRightCode) {
      break;
    } else {
      int x = 0, y = 0;
      int ncode = topLeftCode != 0 ? topLeftCode : bottomRightCode;
      if (ncode & 8) {
        x = newX1 + (newX2 - newX1) * (clipRect.Y2 - newY1) / (newY2 - newY1);
        y = clipRect.Y2;
      } else if (ncode & 4) {
        x = newX1 + (newX2 - newX1) * (clipRect.Y1 - newY1) / (newY2 - newY1);
        y = clipRect.Y1;
      } else if (ncode & 2) {
        y = newY1 + (newY2 - newY1) * (clipRect.X2 - newX1) / (newX2 - newX1);
        x = clipRect.X2;
      } else if (ncode & 1) {
        y = newY1 + (newY2 - newY1) * (clipRect.X1 - newX1) / (newX2 - newX1);
        x = clipRect.X1;
      }
      if (ncode == topLeftCode) {
        newX1 = x;
        newY1 = y;
        topLeftCode = clipLine_code(newX1, newY1, clipRect);
      } else {
        newX2 = x;
        newY2 = y;
        bottomRightCode = clipLine_code(newX2, newY2, clipRect);
      }
    }
  }
  return false;
}


////////////////////////////////////////////////////////////////////////////////////////////
// removeRectangle
// remove "rectToRemove" from "mainRect", pushing remaining rectangles to "rects" stack

void removeRectangle(Stack<Rect> & rects, Rect const & mainRect, Rect const & rectToRemove)
{
  if (!mainRect.intersects(rectToRemove) || rectToRemove.contains(mainRect))
    return;

  // top rectangle
  if (mainRect.Y1 < rectToRemove.Y1)
    rects.push(Rect(mainRect.X1, mainRect.Y1, mainRect.X2, rectToRemove.Y1 - 1));

  // bottom rectangle
  if (mainRect.Y2 > rectToRemove.Y2)
    rects.push(Rect(mainRect.X1, rectToRemove.Y2 + 1, mainRect.X2, mainRect.Y2));

  // left rectangle
  if (mainRect.X1 < rectToRemove.X1)
    rects.push(Rect(mainRect.X1, tmax(rectToRemove.Y1, mainRect.Y1), rectToRemove.X1 - 1, tmin(rectToRemove.Y2, mainRect.Y2)));

  // right rectangle
  if (mainRect.X2 > rectToRemove.X2)
    rects.push(Rect(rectToRemove.X2 + 1, tmax(rectToRemove.Y1, mainRect.Y1), mainRect.X2, tmin(rectToRemove.Y2, mainRect.Y2)));
}


////////////////////////////////////////////////////////////////////////////////////////////
// Rect

Rect IRAM_ATTR Rect::merge(Rect const & rect) const
{
  return Rect(imin(rect.X1, X1), imin(rect.Y1, Y1), imax(rect.X2, X2), imax(rect.Y2, Y2));
}


Rect IRAM_ATTR Rect::intersection(Rect const & rect) const
{
  return Rect(tmax(X1, rect.X1), tmax(Y1, rect.Y1), tmin(X2, rect.X2), tmin(Y2, rect.Y2));
}


///////////////////////////////////////////////////////////////////////////////////
// rgb222_to_hsv
// R, G, B in the 0..3 range
void rgb222_to_hsv(int R, int G, int B, double * h, double * s, double * v)
{
  double r = R / 3.0;
  double g = G / 3.0;
  double b = B / 3.0;
  double cmax = tmax<double>(tmax<double>(r, g), b);
  double cmin = tmin<double>(tmin<double>(r, g), b);
  double diff = cmax - cmin;
  if (cmax == cmin)
    *h = 0;
  else if (cmax == r)
    *h = fmod((60.0 * ((g - b) / diff) + 360.0), 360.0);
  else if (cmax == g)
    *h = fmod((60.0 * ((b - r) / diff) + 120.0), 360.0);
  else if (cmax == b)
    *h = fmod((60.0 * ((r - g) / diff) + 240.0), 360.0);
  *s = cmax == 0 ? 0 : (diff / cmax) * 100.0;
  *v = cmax * 100.0;
}



///////////////////////////////////////////////////////////////////////////////////
// StringList


StringList::StringList()
  : m_items(nullptr),
    m_selMap(nullptr),
    m_ownStrings(false),
    m_count(0),
    m_allocated(0)
{
}


StringList::~StringList()
{
  clear();
}


void StringList::clear()
{
  if (m_ownStrings) {
    for (int i = 0; i < m_count; ++i)
      free((void*) m_items[i]);
  }
  free32(m_items);
  free32(m_selMap);
  m_items  = nullptr;
  m_selMap = nullptr;
  m_count  = m_allocated = 0;
}


void StringList::copyFrom(StringList const & src)
{
  clear();
  m_count = src.m_count;
  checkAllocatedSpace(m_count);
  for (int i = 0; i < m_count; ++i) {
    m_items[i] = nullptr;
    set(i, src.m_items[i]);
  }
  deselectAll();
}


void StringList::copySelectionMapFrom(StringList const & src)
{
  int maskLen = (31 + m_allocated) / 32;
  for (int i = 0; i < maskLen; ++i)
    m_selMap[i] = src.m_selMap[i];
}


void StringList::checkAllocatedSpace(int requiredItems)
{
  if (m_allocated < requiredItems) {
    if (m_allocated == 0) {
      // first time allocates exact space
      m_allocated = requiredItems;
    } else {
      // next times allocate double
      while (m_allocated < requiredItems)
        m_allocated *= 2;
    }
    m_items  = (char const**) realloc32(m_items, m_allocated * sizeof(char const *));
    m_selMap = (uint32_t*) realloc32(m_selMap, (31 + m_allocated) / 32 * sizeof(uint32_t));
  }
}


void StringList::insert(int index, char const * str)
{
  ++m_count;
  checkAllocatedSpace(m_count);
  moveItems(m_items + index + 1, m_items + index, m_count - index - 1);
  m_items[index] = nullptr;
  set(index, str);
  deselectAll();
}


int StringList::append(char const * str)
{
  insert(m_count, str);
  return m_count - 1;
}


int StringList::appendFmt(const char *format, ...)
{
  takeStrings();
  va_list ap;
  va_start(ap, format);
  int size = vsnprintf(nullptr, 0, format, ap) + 1;
  if (size > 0) {
    va_end(ap);
    va_start(ap, format);
    char buf[size + 1];
    vsnprintf(buf, size, format, ap);
    insert(m_count, buf);
  }
  va_end(ap);
  return m_count - 1;
}


void StringList::append(char const * strlist[], int count)
{
  for (int i = 0; i < count; ++i)
    insert(m_count, strlist[i]);
}


// separator cannot be "0"
void StringList::appendSepList(char const * strlist, char separator)
{
  if (strlist) {
    takeStrings();
    char const * start = strlist;
    while (*start) {
      auto end = strchr(start, separator);
      if (!end)
        end = strchr(start, 0);
      int len = end - start;
      char str[len + 1];
      memcpy(str, start, len);
      str[len] = 0;
      insert(m_count, str);
      start += len + (*end == 0 ? 0 : 1);
    }
  }
}


void StringList::set(int index, char const * str)
{
  if (m_ownStrings) {
    free((void*)m_items[index]);
    m_items[index] = (char const*) malloc(strlen(str) + 1);
    strcpy((char*)m_items[index], str);
  } else {
    m_items[index] = str;
  }
}


void StringList::remove(int index)
{
  if (m_ownStrings)
    free((void*)m_items[index]);
  moveItems(m_items + index, m_items + index + 1, m_count - index - 1);
  --m_count;
  deselectAll();
}


void StringList::takeStrings()
{
  if (!m_ownStrings) {
    m_ownStrings = true;
    // take existing strings
    for (int i = 0; i < m_count; ++i) {
      char const * str = m_items[i];
      m_items[i] = nullptr;
      set(i, str);
    }
  }
}


void StringList::deselectAll()
{
  for (int i = 0; i < (31 + m_count) / 32; ++i)
    m_selMap[i] = 0;
}


bool StringList::selected(int index)
{
  return m_selMap[index / 32] & (1 << (index % 32));
}


// -1 = no items selected
int StringList::getFirstSelected()
{
  for (int i = 0; i < m_count; ++i)
    if (selected(i))
      return i;
  return -1;
}


void StringList::select(int index, bool value)
{
  if (value)
    m_selMap[index / 32] |= 1 << (index % 32);
  else
    m_selMap[index / 32] &= ~(1 << (index % 32));
}



// StringList
///////////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////////
// FileBrowser


char const *   FileBrowser::s_SPIFFSMountPath;
bool           FileBrowser::s_SPIFFSMounted = false;
size_t         FileBrowser::s_SPIFFSMaxFiles;

char const *   FileBrowser::s_SDCardMountPath;
bool           FileBrowser::s_SDCardMounted = false;
size_t         FileBrowser::s_SDCardMaxFiles;
int            FileBrowser::s_SDCardAllocationUnitSize;
int8_t         FileBrowser::s_SDCardMISO;
int8_t         FileBrowser::s_SDCardMOSI;
int8_t         FileBrowser::s_SDCardCLK;
int8_t         FileBrowser::s_SDCardCS;
sdmmc_card_t * FileBrowser::s_SDCard = nullptr;



FileBrowser::FileBrowser()
  : m_dir(nullptr),
    m_count(0),
    m_items(nullptr),
    m_sorted(true),
    m_includeHiddenFiles(false),
    m_namesStorage(nullptr)
{
}


FileBrowser::FileBrowser(char const * path)
  : FileBrowser()
{
  setDirectory(path);
}


FileBrowser::~FileBrowser()
{
  clear();

  free(m_dir);
  m_dir = nullptr;
}


void FileBrowser::clear()
{
  free(m_items);
  m_items = nullptr;

  free(m_namesStorage);
  m_namesStorage = nullptr;

  m_count = 0;
}


// set absolute directory (full path must be specified)
bool FileBrowser::setDirectory(const char * path)
{
  if (m_dir == nullptr || strcmp(path, m_dir) != 0) {
    free(m_dir);
    m_dir = strdup(path);
  }
  return reload();
}


// set relative directory:
//   ".." : go to the parent directory
//   "dirname": go inside the specified sub directory
void FileBrowser::changeDirectory(const char * subdir)
{
  if (!m_dir || strlen(subdir) == 0)
    return;
  if (strcmp(subdir, "..") == 0) {
    // go to parent directory
    auto lastSlash = strrchr(m_dir, '/');
    if (lastSlash) {
      if (lastSlash != m_dir)
        lastSlash[0] = 0;
      else
        lastSlash[1] = 0;
      reload();
    }
  } else {
    // go to sub directory
    auto oldLen = strcmp(m_dir, "/") == 0 ? 0 : strlen(m_dir);
    char * newDir = (char*) malloc(oldLen + 1 + strlen(subdir) + 1);  // m_dir + '/' + subdir + 0
    strcpy(newDir, m_dir);
    newDir[oldLen] = '/';
    strcpy(newDir + oldLen + 1, subdir);
    free(m_dir);
    m_dir = newDir;
    reload();
  }
}


int FileBrowser::countDirEntries(int * namesLength)
{
  return 0;
}


bool FileBrowser::exists(char const * name, bool caseSensitive)
{
  return false;
}


bool FileBrowser::filePathExists(char const * filepath)
{
  return false;
}


size_t FileBrowser::fileSize(char const * name)
{
  return 0;
}


bool FileBrowser::fileCreationDate(char const * name, int * year, int * month, int * day, int * hour, int * minutes, int * seconds)
{
  char fullpath[strlen(m_dir) + 1 + strlen(name) + 1];
  sprintf(fullpath, "%s/%s", m_dir, name);
  struct stat s;
  if (stat(fullpath, &s))
    return false;
  auto tm  = *localtime((time_t*)&s.st_ctime);  // I know, this is not create date, but status change. Anyway I cannot find "st_birthtimespec"
  *year    = 1900 + tm.tm_year;
  *month   = 1 + tm.tm_mon;
  *day     = tm.tm_mday;
  *hour    = tm.tm_hour;
  *minutes = tm.tm_min;
  *seconds = imin(tm.tm_sec, 59); // [0, 61] (until C99), [0, 60] (since C99)
  return true;
}


bool FileBrowser::fileUpdateDate(char const * name, int * year, int * month, int * day, int * hour, int * minutes, int * seconds)
{
  char fullpath[strlen(m_dir) + 1 + strlen(name) + 1];
  sprintf(fullpath, "%s/%s", m_dir, name);
  struct stat s;
  if (stat(fullpath, &s))
    return false;
  auto tm  = *localtime((time_t*)&s.st_mtime);
  *year    = 1900 + tm.tm_year;
  *month   = 1 + tm.tm_mon;
  *day     = tm.tm_mday;
  *hour    = tm.tm_hour;
  *minutes = tm.tm_min;
  *seconds = imin(tm.tm_sec, 59); // [0, 61] (until C99), [0, 60] (since C99)
  return true;
}


bool FileBrowser::fileAccessDate(char const * name, int * year, int * month, int * day, int * hour, int * minutes, int * seconds)
{
  char fullpath[strlen(m_dir) + 1 + strlen(name) + 1];
  sprintf(fullpath, "%s/%s", m_dir, name);
  struct stat s;
  if (stat(fullpath, &s))
    return false;
  auto tm  = *localtime((time_t*)&s.st_atime);
  *year    = 1900 + tm.tm_year;
  *month   = 1 + tm.tm_mon;
  *day     = tm.tm_mday;
  *hour    = tm.tm_hour;
  *minutes = tm.tm_min;
  *seconds = imin(tm.tm_sec, 59); // [0, 61] (until C99), [0, 60] (since C99)
  return true;
}



int DirComp(const void * i1, const void * i2)
{
  DirItem * d1 = (DirItem*)i1;
  DirItem * d2 = (DirItem*)i2;
  if (d1->isDir != d2->isDir) // directories first
    return d1->isDir ? -1 : +1;
  else
    return strcmp(d1->name, d2->name);
}


bool FileBrowser::reload()
{
  return 0;
}


// note: for SPIFFS this creates an empty ".dirname" file. The SPIFFS path is detected when path starts with "/spiffs" or s_SPIFFSMountPath
// "dirname" is not a path, just a directory name created inside "m_dir"
void FileBrowser::makeDirectory(char const * dirname)
{
}


// removes a file or a directory (and all files inside it)
// The SPIFFS path is detected when path starts with "/spiffs" or s_SPIFFSMountPath
// "name" is not a path, just a file or directory name inside "m_dir"
void FileBrowser::remove(char const * name)
{
}


// works only for files
void FileBrowser::rename(char const * oldName, char const * newName)
{
  char oldfullpath[strlen(m_dir) + 1 + strlen(oldName) + 1];
  sprintf(oldfullpath, "%s/%s", m_dir, oldName);

  char newfullpath[strlen(m_dir) + 1 + strlen(newName) + 1];
  sprintf(newfullpath, "%s/%s", m_dir, newName);

  ::rename(oldfullpath, newfullpath);
}


// return a full path
char * FileBrowser::createTempFilename()
{
  constexpr int FLEN = 6;
  auto ret = (char*) malloc(strlen(m_dir) + 1 + FLEN + 4 + 1);
  while (true) {
    char name[FLEN + 1] = { 0 };
    for (int i = 0; i < FLEN; ++i)
      name[i] = 65 + (rand() % 26);
    sprintf(ret, "%s/%s.TMP", m_dir, name);
    if (!exists(name, false))
      return ret;
  }
}


bool FileBrowser::truncate(char const * name, size_t size)
{
  constexpr size_t BUFLEN = 512;

  char fullpath[strlen(m_dir) + 1 + strlen(name) + 1];
  sprintf(fullpath, "%s/%s", m_dir, name);

  // in future maybe...
  //::truncate(name, size);

  bool retval = false;

  // for now...
  char * tempFilename = createTempFilename();
  if (::rename(fullpath, tempFilename) == 0) {
    void * buf = malloc(BUFLEN);
    if (buf) {
      auto fr = fopen(tempFilename, "rb");
      if (fr) {
        auto fw = fopen(fullpath, "wb");
        if (fw) {

          while (size > 0) {
            auto l = fread(buf, 1, tmin(size, BUFLEN), fr);
            if (l == 0)
              break;
            fwrite(buf, 1, l, fw);
            size -= l;
          }

          // just in case truncate is used to expand the file
          for (; size > 0; --size)
            fputc(0, fw);

          retval = true;
          fclose(fw);
        }
        fclose(fr);
      }
    }
    free(buf);
    unlink(tempFilename);
  }
  free(tempFilename);
  return retval;
}


// concatenates current directory and specified name and store result into fullpath
// Specifying outPath=nullptr returns required length
int FileBrowser::getFullPath(char const * name, char * outPath, int maxlen)
{
  return (outPath ? snprintf(outPath, maxlen, "%s/%s", m_dir, name) : snprintf(nullptr, 0, "%s/%s", m_dir, name)) + 1;
}


FILE * FileBrowser::openFile(char const * filename, char const * mode)
{
  char fullpath[strlen(m_dir) + 1 + strlen(filename) + 1];
  strcpy(fullpath, m_dir);
  strcat(fullpath, "/");
  strcat(fullpath, filename);

  replacePathSep(fullpath, '/');

  return fopen(fullpath, mode);
}


DriveType FileBrowser::getCurrentDriveType()
{
  return getDriveType(m_dir);
}


DriveType FileBrowser::getDriveType(char const * path)
{
  if (strncmp(path, "/spiffs", 7) == 0 || (s_SPIFFSMounted && strncmp(path, s_SPIFFSMountPath, strlen(s_SPIFFSMountPath)) == 0)) {
    return DriveType::SPIFFS;
  } else if (s_SDCardMounted && strncmp(path, s_SDCardMountPath, strlen(s_SDCardMountPath)) == 0) {
    return DriveType::SDCard;
  } else {
    return DriveType::None;
  }
}


bool FileBrowser::format(DriveType driveType, int drive)
{
  esp_task_wdt_init(45, false);

  if (driveType == DriveType::SDCard && s_SDCardMounted) {

    // unmount filesystem
    char drv[3] = {(char)('0' + drive), ':', 0};
    f_mount(0, drv, 0);

    void * buffer = malloc(FF_MAX_SS);
    if (!buffer)
      return false;

    // create partition
    DWORD plist[] = { 100, 0, 0, 0 };
    if (f_fdisk(drive, plist, buffer) != FR_OK) {
      free(buffer);
      return false;
    }

    // make filesystem
    if (f_mkfs(drv, FM_ANY, 16 * 1024, buffer, FF_MAX_SS) != FR_OK) {
      free(buffer);
      return false;
    }

    free(buffer);

    remountSDCard();

    return true;

  } else if (driveType == DriveType::SPIFFS && s_SPIFFSMounted) {

    // driveType == DriveType::SPIFFS
    bool r = (esp_spiffs_format(nullptr) == ESP_OK);

    remountSPIFFS();

    return r;

  } else
    return false;
}


bool FileBrowser::mountSDCard(bool formatOnFail, char const * mountPath, size_t maxFiles, int allocationUnitSize, int MISO, int MOSI, int CLK, int CS)
{
  switch (getChipPackage()) {
    case ChipPackage::ESP32PICOD4:
      MISO = 2;
      MOSI = 12;
      break;
    case ChipPackage::ESP32D0WDQ5:
      MISO = 35;
      MOSI = 12;
      break;
    default:
      break;
  }

  s_SDCardMountPath          = mountPath;
  s_SDCardMaxFiles           = maxFiles;
  s_SDCardAllocationUnitSize = allocationUnitSize;
  s_SDCardMISO               = MISO;
  s_SDCardMOSI               = MOSI;
  s_SDCardCLK                = CLK;
  s_SDCardCS                 = CS;
  s_SDCardMounted            = false;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = HSPI_HOST;

  #if FABGL_ESP_IDF_VERSION <= FABGL_ESP_IDF_VERSION_VAL(3, 3, 5)

  sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
  slot_config.gpio_miso = int2gpio(MISO);
  slot_config.gpio_mosi = int2gpio(MOSI);
  slot_config.gpio_sck  = int2gpio(CLK);
  slot_config.gpio_cs   = int2gpio(CS);

  esp_vfs_fat_sdmmc_mount_config_t mount_config;
  mount_config.format_if_mount_failed = formatOnFail;
  mount_config.max_files              = maxFiles;
  mount_config.allocation_unit_size   = allocationUnitSize;

  s_SDCardMounted = (esp_vfs_fat_sdmmc_mount(mountPath, &host, &slot_config, &mount_config, &s_SDCard) == ESP_OK);

  #else

  // slow down SD card. Using ESP32 core 2.0.0 may crash SD subsystem, having VGA output and WiFi enabled
  // @TODO: check again
  host.max_freq_khz = 19000;

  spi_bus_config_t bus_cfg = {
        .mosi_io_num = int2gpio(MOSI),
        .miso_io_num = int2gpio(MISO),
        .sclk_io_num = int2gpio(CLK),
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
  };
  auto r = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, 2);

  if (r == ESP_OK || r == ESP_ERR_INVALID_STATE) {  // ESP_ERR_INVALID_STATE, maybe spi_bus_initialize already called
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = int2gpio(CS);
    slot_config.host_id = (spi_host_device_t) host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config;
    mount_config.format_if_mount_failed = formatOnFail;
    mount_config.max_files              = maxFiles;
    mount_config.allocation_unit_size   = allocationUnitSize;

    r = esp_vfs_fat_sdspi_mount(mountPath, &host, &slot_config, &mount_config, &s_SDCard);

    s_SDCardMounted = (r == ESP_OK);
  }

  #endif

  return s_SDCardMounted;
}


void FileBrowser::unmountSDCard()
{
  if (s_SDCardMounted) {
    #if FABGL_ESP_IDF_VERSION <= FABGL_ESP_IDF_VERSION_VAL(3, 3, 5)
    esp_vfs_fat_sdmmc_unmount();
    #else
    esp_vfs_fat_sdcard_unmount(s_SDCardMountPath, s_SDCard);
    #endif
    s_SDCardMounted = false;
  }
}


bool FileBrowser::remountSDCard()
{
  unmountSDCard();
  return mountSDCard(false, s_SDCardMountPath, s_SDCardMaxFiles, s_SDCardAllocationUnitSize, s_SDCardMISO, s_SDCardMOSI, s_SDCardCLK, s_SDCardCS);
}


bool FileBrowser::mountSPIFFS(bool formatOnFail, char const * mountPath, size_t maxFiles)
{
  s_SPIFFSMountPath = mountPath;
  s_SPIFFSMaxFiles  = maxFiles;
  esp_vfs_spiffs_conf_t conf = {
      .base_path              = mountPath,
      .partition_label        = nullptr,
      .max_files              = maxFiles,
      .format_if_mount_failed = true
  };
  s_SPIFFSMounted = (esp_vfs_spiffs_register(&conf) == ESP_OK);
  return s_SPIFFSMounted;
}


void FileBrowser::unmountSPIFFS()
{
  if (s_SPIFFSMounted) {
    esp_vfs_spiffs_unregister(nullptr);
    s_SPIFFSMounted = false;
  }
}


bool FileBrowser::remountSPIFFS()
{
  unmountSPIFFS();
  return mountSPIFFS(false, s_SPIFFSMountPath, s_SPIFFSMaxFiles);
}


bool FileBrowser::getFSInfo(DriveType driveType, int drive, int64_t * total, int64_t * used)
{
  *total = *used = 0;

  if (driveType == DriveType::SDCard) {

    FATFS * fs;
    DWORD free_clusters;
    char drv[3] = {(char)('0' + drive), ':', 0};
    if (f_getfree(drv, &free_clusters, &fs) != FR_OK)
      return false;
    int64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    int64_t free_sectors = free_clusters * fs->csize;
    *total = total_sectors * fs->ssize;
    *used = *total - free_sectors * fs->ssize;

    return true;

  } else if (driveType == DriveType::SPIFFS) {

    size_t stotal = 0, sused = 0;
    if (esp_spiffs_info(NULL, &stotal, &sused) != ESP_OK)
      return false;
    *total = stotal;
    *used  = sused;
    return true;

  } else
    return false;
}


// FileBrowser
///////////////////////////////////////////////////////////////////////////////////




///////////////////////////////////////////////////////////////////////////////////
// LightMemoryPool


void LightMemoryPool::mark(int pos, int16_t size, bool allocated)
{
  m_mem[pos]     = size & 0xff;
  m_mem[pos + 1] = ((size >> 8) & 0x7f) | (allocated ? 0x80 : 0);
}


int16_t LightMemoryPool::getSize(int pos)
{
  return m_mem[pos] | ((m_mem[pos + 1] & 0x7f) << 8);
}


bool LightMemoryPool::isFree(int pos)
{
  return (m_mem[pos + 1] & 0x80) == 0;
}


LightMemoryPool::LightMemoryPool(int poolSize)
{
  m_poolSize = poolSize + 2;
  m_mem = (uint8_t*) heap_caps_malloc(m_poolSize, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  mark(0, m_poolSize - 2, false);
}


LightMemoryPool::~LightMemoryPool()
{
  heap_caps_free(m_mem);
}


void * LightMemoryPool::alloc(int size)
{
  for (int pos = 0; pos < m_poolSize; ) {
    int16_t blockSize = getSize(pos);
    if (isFree(pos)) {
      if (blockSize == size) {
        // found a block having the same size
        mark(pos, size, true);
        return m_mem + pos + 2;
      } else if (blockSize > size) {
        // found a block having larger size
        int remainingSize = blockSize - size - 2;
        if (remainingSize > 0)
          mark(pos + 2 + size, remainingSize, false);  // create new free block at the end of this block
        else
          size = blockSize; // to avoid to waste last block
        mark(pos, size, true);  // reduce size of this block and mark as allocated
        return m_mem + pos + 2;
      } else {
        // this block hasn't enough space
        // can merge with next block?
        int nextBlockPos = pos + 2 + blockSize;
        if (nextBlockPos < m_poolSize && isFree(nextBlockPos)) {
          // join blocks and stay at this pos
          mark(pos, blockSize + getSize(nextBlockPos) + 2, false);
        } else {
          // move to the next block
          pos += blockSize + 2;
        }
      }
    } else {
      // move to the next block
      pos += blockSize + 2;
    }
  }
  return nullptr;
}


bool LightMemoryPool::memCheck()
{
  int pos = 0;
  while (pos < m_poolSize) {
    int16_t blockSize = getSize(pos);
    pos += blockSize + 2;
  }
  return pos == m_poolSize;
}


int LightMemoryPool::totFree()
{
  int r = 0;
  for (int pos = 0; pos < m_poolSize; ) {
    int16_t blockSize = getSize(pos);
    if (isFree(pos))
      r += blockSize;
    pos += blockSize + 2;
  }
  return r;
}


int LightMemoryPool::totAllocated()
{
  int r = 0;
  for (int pos = 0; pos < m_poolSize; ) {
    int16_t blockSize = getSize(pos);
    if (!isFree(pos))
      r += blockSize;
    pos += blockSize + 2;
  }
  return r;
}


int LightMemoryPool::largestFree()
{
  int r = 0;
  for (int pos = 0; pos < m_poolSize; ) {
    int16_t blockSize = getSize(pos);
    if (isFree(pos) && blockSize > r)
      r = blockSize;
    pos += blockSize + 2;
  }
  return r;
}


// LightMemoryPool
///////////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////////
// CoreUsage


int CoreUsage::s_busiestCore = FABGLIB_VIDEO_CPUINTENSIVE_TASKS_CORE;


// CoreUsage
///////////////////////////////////////////////////////////////////////////////////




}

