/*
 * Copyright (C) 2014-2015  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "bootimage.h"

#include <algorithm>

#include <cstring>

#include "libmbpio/file.h"

#include "bootimage/androidformat.h"
#include "bootimage/bumpformat.h"
#include "bootimage/lokiformat.h"
#include "bootimage/sonyelfformat.h"

#include "external/sha.h"
#include "private/fileutils.h"
#include "private/logging.h"


namespace mbp
{

const char *BootImage::BootMagic = BOOT_MAGIC;
const uint32_t BootImage::BootMagicSize = BOOT_MAGIC_SIZE;
const uint32_t BootImage::BootNameSize = BOOT_NAME_SIZE;
const uint32_t BootImage::BootArgsSize = BOOT_ARGS_SIZE;

const char *BootImage::DefaultBoard = "";
const char *BootImage::DefaultCmdline = "";
const uint32_t BootImage::DefaultPageSize = 2048u;
const uint32_t BootImage::DefaultBase = 0x10000000u;
const uint32_t BootImage::DefaultKernelOffset = 0x00008000u;
const uint32_t BootImage::DefaultRamdiskOffset = 0x01000000u;
const uint32_t BootImage::DefaultSecondOffset = 0x00f00000u;
const uint32_t BootImage::DefaultTagsOffset = 0x00000100u;
const uint32_t BootImage::DefaultIplAddress = 0u;
const uint32_t BootImage::DefaultRpmAddress = 0u;
const uint32_t BootImage::DefaultAppsblAddress = 0u;
const uint32_t BootImage::DefaultEntrypointAddress = 0u;


/*! \cond INTERNAL */
class BootImage::Impl
{
public:
    Impl(BootImage *parent) : m_parent(parent) {}

    BootImageIntermediate i10e;
    BootImage::Type type = Type::Android;
    BootImage::Type sourceType;

    PatcherError error;

private:
    BootImage *m_parent;
};
/*! \endcond */


/*!
 * \class BootImage
 * \brief Handles the creation and manipulation of Android boot images
 *
 * BootImage provides a complete implementation of the following formats:
 *
 * | Format           | Extract | Create |
 * |------------------|---------|--------|
 * | Android          | Yes     | Yes    |
 * | Loki (old-style) | Yes     | No     | (Will be created as new-style)
 * | Loki (new-style) | Yes     | Yes    |
 * | Bump             | Yes     | Yes    |
 *
 * The following parameters in the Android header can be changed:
 *
 * - Board name (truncated if length > 16)
 * - Kernel cmdline (truncated if length > 512)
 * - Page size
 * - Kernel address [1]
 * - Ramdisk address [1]
 * - Second bootloader address [1]
 * - Kernel tags address [1]
 * - Kernel size [2]
 * - Ramdisk size [2]
 * - Second bootloader size [2]
 * - Device tree size [2]
 * - SHA1 identifier [3]
 *
 * [1] - Can be set using a base and an offset
 *
 * ]2] - Cannot be manually changed. This is automatically updated when the
 *       corresponding image is set
 *
 * [3] - This is automatically computed when the images within the boot image
 *       are changed
 *
 *
 * If the boot image is patched with loki, the following parameters may be used:
 *
 * - Original kernel size
 * - Original ramdisk size
 * - Ramdisk address
 *
 * However, because some of these parameters were set to zero in early versions
 * of loki, they are sometimes ignored and BootImage will search the file for
 * the location of the kernel image and ramdisk image.
 */

BootImage::BootImage() : m_impl(new Impl(this))
{
    // Initialize to sane defaults
    resetKernelCmdline();
    resetBoardName();
    resetKernelAddress();
    resetRamdiskAddress();
    resetSecondBootloaderAddress();
    resetKernelTagsAddress();
    resetIplAddress();
    resetRpmAddress();
    resetAppsblAddress();
    resetEntrypointAddress();
    resetPageSize();
    // Prevent valgrind warning about uninitialized bytes when writing file
    m_impl->i10e.hdrUnused = 0;
}

BootImage::~BootImage()
{
}

/*!
 * \brief Get error information
 *
 * \note The returned PatcherError contains valid information only if an
 *       operation has failed.
 *
 * \return PatcherError containing information about the error
 */
PatcherError BootImage::error() const
{
    return m_impl->error;
}

bool BootImage::load(const unsigned char *data, std::size_t size)
{
    bool ret = false;

    if (LokiFormat::isValid(data, size)) {
        LOGD("Boot image is a loki'd Android boot image");
        m_impl->sourceType = Type::Loki;
        ret = LokiFormat(&m_impl->i10e).loadImage(data, size);
    } else if (BumpFormat::isValid(data, size)) {
        LOGD("Boot image is a bump'd Android boot image");
        m_impl->sourceType = Type::Bump;
        ret = BumpFormat(&m_impl->i10e).loadImage(data, size);
    } else if (AndroidFormat::isValid(data, size)) {
        LOGD("Boot image is a plain boot image");
        m_impl->sourceType = Type::Android;
        ret = AndroidFormat(&m_impl->i10e).loadImage(data, size);
    } else if (SonyElfFormat::isValid(data, size)) {
        LOGD("Boot image is a Sony ELF32 boot image");
        m_impl->sourceType = Type::SonyElf;
        ret = SonyElfFormat(&m_impl->i10e).loadImage(data, size);
    } else {
        LOGD("Unknown boot image type");
    }

    if (!ret) {
        m_impl->error = PatcherError::createBootImageError(
                ErrorCode::BootImageParseError);
        return false;
    }

    return true;
}

/*!
 * \brief Load a boot image from binary data
 *
 * This function loads a boot image from a vector containing the binary data.
 * The boot image headers and other images (eg. kernel and ramdisk) will be
 * copied and stored.
 *
 * \warning If the boot image cannot be loaded, do not use the same BootImage
 *          object to load another boot image as it may contain partially
 *          loaded data.
 *
 * \return Whether the boot image was successfully read and parsed.
 */
bool BootImage::load(const std::vector<unsigned char> &data)
{
    return load(data.data(), data.size());
}

/*!
 * \brief Load a boot image file
 *
 * This function reads a boot image file and then calls
 * BootImage::load(const std::vector<unsigned char> &)
 *
 * \warning If the boot image cannot be loaded, do not use the same BootImage
 *          object to load another boot image as it may contain partially
 *          loaded data.
 *
 * \sa BootImage::load(const std::vector<unsigned char> &)
 *
 * \return Whether the boot image was successfully read and parsed.
 */
bool BootImage::loadFile(const std::string &filename)
{
    std::vector<unsigned char> data;
    auto ret = FileUtils::readToMemory(filename, &data);
    if (!ret) {
        m_impl->error = ret;
        return false;
    }

    return load(data);
}

/*!
 * \brief Constructs the boot image binary data
 *
 * This function builds the bootable boot image binary data that the BootImage
 * represents. This is equivalent to AOSP's \a mkbootimg tool.
 *
 * \return Boot image binary data
 */
bool BootImage::create(std::vector<unsigned char> *data) const
{
    bool ret = false;

    switch (m_impl->type) {
    case Type::Android:
        LOGD("Creating Android boot image");
        ret = AndroidFormat(&m_impl->i10e).createImage(data);
        break;
    case Type::Bump:
        LOGD("Creating bump'd Android boot image");
        ret = BumpFormat(&m_impl->i10e).createImage(data);
        break;
    case Type::Loki:
        LOGD("Creating loki'd Android boot image");
        ret = LokiFormat(&m_impl->i10e).createImage(data);
        break;
    case Type::SonyElf:
        LOGD("Creating Sony ELF32 boot image");
        ret = SonyElfFormat(&m_impl->i10e).createImage(data);
        break;
    default:
        LOGE("Unknown boot image type");
        break;
    }

    return ret;
}

/*!
 * \brief Constructs boot image and writes it to a file
 *
 * This is a convenience function that calls BootImage::create() and writes the
 * data to the specified file.
 *
 * \return Whether the file was successfully written
 *
 * \sa BootImage::create()
 */
bool BootImage::createFile(const std::string &path)
{
    io::File file;
    if (!file.open(path, io::File::OpenWrite)) {
        FLOGE("%s: Failed to open for writing: %s",
              path.c_str(), file.errorString().c_str());

        m_impl->error = PatcherError::createIOError(
                ErrorCode::FileOpenError, path);
        return false;
    }

    std::vector<unsigned char> data;
    if (!create(&data)) {
        return false;
    }

    uint64_t bytesWritten;
    if (!file.write(data.data(), data.size(), &bytesWritten)) {
        FLOGE("%s: Failed to write file: %s",
              path.c_str(), file.errorString().c_str());

        m_impl->error = PatcherError::createIOError(
                ErrorCode::FileWriteError, path);
        return false;
    }

    return true;
}

/*!
 * \brief Get type of boot image
 *
 * This is set to the type of the source boot image if it has not been changed
 * by calling setFormat().
 *
 * \note The return value is undefined before load() or loadFile() has been
 *       called (and returned true).
 *
 * \return Boot image format
 */
BootImage::Type BootImage::wasType() const
{
    return m_impl->sourceType;
}

void BootImage::setType(BootImage::Type type)
{
    m_impl->type = type;
}

////////////////////////////////////////////////////////////////////////////////
// Board name
////////////////////////////////////////////////////////////////////////////////

/*!
 * \brief Board name field in the boot image header
 *
 * \return Board name
 */
const std::string & BootImage::boardName() const
{
    return m_impl->i10e.boardName;
}

/*!
 * \brief Set the board name field in the boot image header
 *
 * \param name Board name
 */
void BootImage::setBoardName(std::string name)
{
    m_impl->i10e.boardName = std::move(name);
}

const char * BootImage::boardNameC() const
{
    return m_impl->i10e.boardName.c_str();
}

void BootImage::setBoardNameC(const char *name)
{
    m_impl->i10e.boardName = name;
}

/*!
 * \brief Resets the board name field in the boot image header to the default
 *
 * The board name field is empty by default.
 */
void BootImage::resetBoardName()
{
    setBoardName(DefaultBoard);
}

////////////////////////////////////////////////////////////////////////////////
// Kernel cmdline
////////////////////////////////////////////////////////////////////////////////

/*!
 * \brief Kernel cmdline in the boot image header
 *
 * \return Kernel cmdline
 */
const std::string & BootImage::kernelCmdline() const
{
    return m_impl->i10e.cmdline;
}

/*!
 * \brief Set the kernel cmdline in the boot image header
 *
 * \param cmdline Kernel cmdline
 */
void BootImage::setKernelCmdline(std::string cmdline)
{
    m_impl->i10e.cmdline = std::move(cmdline);
}

const char * BootImage::kernelCmdlineC() const
{
    return m_impl->i10e.cmdline.c_str();
}

void BootImage::setKernelCmdlineC(const char *cmdline)
{
    m_impl->i10e.cmdline = cmdline;
}

/*!
 * \brief Resets the kernel cmdline to the default
 *
 * The kernel cmdline is empty by default.
 */
void BootImage::resetKernelCmdline()
{
    setKernelCmdline(DefaultCmdline);
}

////////////////////////////////////////////////////////////////////////////////

/*!
 * \brief Page size field in the boot image header
 *
 * \return Page size
 */
uint32_t BootImage::pageSize() const
{
    return m_impl->i10e.pageSize;
}

/*!
 * \brief Set the page size field in the boot image header
 *
 * \note The page size should be one of if 2048, 4096, 8192, 16384, 32768,
 *       65536, or 131072
 *
 * \param size Page size
 */
void BootImage::setPageSize(uint32_t size)
{
    m_impl->i10e.pageSize = size;
}

/*!
 * \brief Resets the page size field in the header to the default
 *
 * The default page size is 2048 bytes.
 */
void BootImage::resetPageSize()
{
    setPageSize(DefaultPageSize);
}

/*!
 * \brief Kernel address field in the boot image header
 *
 * \return Kernel address
 */
uint32_t BootImage::kernelAddress() const
{
    return m_impl->i10e.kernelAddr;
}

/*!
 * \brief Set the kernel address field in the boot image header
 *
 * \param address Kernel address
 */
void BootImage::setKernelAddress(uint32_t address)
{
    m_impl->i10e.kernelAddr = address;
}

/*!
 * \brief Resets the kernel address field in the header to the default
 *
 * The default kernel address is 0x10000000 + 0x00008000.
 */
void BootImage::resetKernelAddress()
{
    setKernelAddress(DefaultBase + DefaultKernelOffset);
}

/*!
 * \brief Ramdisk address field in the boot image header
 *
 * \return Ramdisk address
 */
uint32_t BootImage::ramdiskAddress() const
{
    return m_impl->i10e.ramdiskAddr;
}

/*!
 * \brief Set the ramdisk address field in the boot image header
 *
 * \param address Ramdisk address
 */
void BootImage::setRamdiskAddress(uint32_t address)
{
    m_impl->i10e.ramdiskAddr = address;
}

/*!
 * \brief Resets the ramdisk address field in the header to the default
 *
 * The default ramdisk address is 0x10000000 + 0x01000000.
 */
void BootImage::resetRamdiskAddress()
{
    setRamdiskAddress(DefaultBase + DefaultRamdiskOffset);
}

/*!
 * \brief Second bootloader address field in the boot image header
 *
 * \return Second bootloader address
 */
uint32_t BootImage::secondBootloaderAddress() const
{
    return m_impl->i10e.secondAddr;
}

/*!
 * \brief Set the second bootloader address field in the boot image header
 *
 * \param address Second bootloader address
 */
void BootImage::setSecondBootloaderAddress(uint32_t address)
{
    m_impl->i10e.secondAddr = address;
}

/*!
 * \brief Resets the second bootloader address field in the header to the default
 *
 * The default second bootloader address is 0x10000000 + 0x00f00000.
 */
void BootImage::resetSecondBootloaderAddress()
{
    setSecondBootloaderAddress(DefaultBase + DefaultSecondOffset);
}

/*!
 * \brief Kernel tags address field in the boot image header
 *
 * \return Kernel tags address
 */
uint32_t BootImage::kernelTagsAddress() const
{
    return m_impl->i10e.tagsAddr;
}

/*!
 * \brief Set the kernel tags address field in the boot image header
 *
 * \param address Kernel tags address
 */
void BootImage::setKernelTagsAddress(uint32_t address)
{
    m_impl->i10e.tagsAddr = address;
}

/*!
 * \brief Resets the kernel tags address field in the header to the default
 *
 * The default kernel tags address is 0x10000000 + 0x00000100.
 */
void BootImage::resetKernelTagsAddress()
{
    setKernelTagsAddress(DefaultBase + DefaultTagsOffset);
}

uint32_t BootImage::iplAddress() const
{
    return m_impl->i10e.iplAddr;
}

void BootImage::setIplAddress(uint32_t address)
{
    m_impl->i10e.iplAddr = address;
}

void BootImage::resetIplAddress()
{
    setIplAddress(DefaultIplAddress);
}

uint32_t BootImage::rpmAddress() const
{
    return m_impl->i10e.rpmAddr;
}

void BootImage::setRpmAddress(uint32_t address)
{
    m_impl->i10e.rpmAddr = address;
}

void BootImage::resetRpmAddress()
{
    setRpmAddress(DefaultRpmAddress);
}

uint32_t BootImage::appsblAddress() const
{
    return m_impl->i10e.appsblAddr;
}

void BootImage::setAppsblAddress(uint32_t address)
{
    m_impl->i10e.appsblAddr = address;
}

void BootImage::resetAppsblAddress()
{
    setAppsblAddress(DefaultAppsblAddress);
}

uint32_t BootImage::entrypointAddress() const
{
    return m_impl->i10e.hdrEntrypoint;
}

void BootImage::setEntrypointAddress(uint32_t address)
{
    m_impl->i10e.hdrEntrypoint = address;
}

void BootImage::resetEntrypointAddress()
{
    setEntrypointAddress(DefaultEntrypointAddress);
}

/*!
 * \brief Set all of the addresses using offsets and a base address
 *
 * - `[Kernel address] = [Base] + [Kernel offset]`
 * - `[Ramdisk address] = [Base] + [Ramdisk offset]`
 * - `[Second bootloader address] = [Base] + [Second bootloader offset]`
 * - `[Kernel tags address] = [Base] + [Kernel tags offset]`
 *
 * \param base Base address
 * \param kernelOffset Kernel offset
 * \param ramdiskOffset Ramdisk offset
 * \param secondBootloaderOffset Second bootloader offset
 * \param kernelTagsOffset Kernel tags offset
 */
void BootImage::setAddresses(uint32_t base, uint32_t kernelOffset,
                             uint32_t ramdiskOffset,
                             uint32_t secondBootloaderOffset,
                             uint32_t kernelTagsOffset)
{
    setKernelAddress(base + kernelOffset);
    setRamdiskAddress(base + ramdiskOffset);
    setSecondBootloaderAddress(base + secondBootloaderOffset);
    setKernelTagsAddress(base + kernelTagsOffset);
}

////////////////////////////////////////////////////////////////////////////////
// Kernel image
////////////////////////////////////////////////////////////////////////////////

/*!
 * \brief Kernel image
 *
 * \return Vector containing the kernel image binary data
 */
const std::vector<unsigned char> & BootImage::kernelImage() const
{
    return m_impl->i10e.kernelImage;
}

/*!
 * \brief Set the kernel image
 *
 * This will automatically update the kernel size in the boot image header and
 * recalculate the SHA1 hash.
 */
void BootImage::setKernelImage(std::vector<unsigned char> data)
{
    m_impl->i10e.hdrKernelSize = data.size();
    m_impl->i10e.kernelImage = std::move(data);
}

void BootImage::kernelImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.kernelImage.data();
    *size = m_impl->i10e.kernelImage.size();
}

void BootImage::setKernelImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.kernelImage.clear();
    m_impl->i10e.kernelImage.shrink_to_fit();
    m_impl->i10e.kernelImage.resize(size);
    std::memcpy(m_impl->i10e.kernelImage.data(), data, size);
    m_impl->i10e.hdrKernelSize = size;
}

////////////////////////////////////////////////////////////////////////////////
// Ramdisk image
////////////////////////////////////////////////////////////////////////////////

/*!
 * \brief Ramdisk image
 *
 * \return Vector containing the ramdisk image binary data
 */
const std::vector<unsigned char> & BootImage::ramdiskImage() const
{
    return m_impl->i10e.ramdiskImage;
}

/*!
 * \brief Set the ramdisk image
 *
 * This will automatically update the ramdisk size in the boot image header and
 * recalculate the SHA1 hash.
 */
void BootImage::setRamdiskImage(std::vector<unsigned char> data)
{
    m_impl->i10e.hdrRamdiskSize = data.size();
    m_impl->i10e.ramdiskImage = std::move(data);
}

void BootImage::ramdiskImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.ramdiskImage.data();
    *size = m_impl->i10e.ramdiskImage.size();
}

void BootImage::setRamdiskImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.ramdiskImage.clear();
    m_impl->i10e.ramdiskImage.shrink_to_fit();
    m_impl->i10e.ramdiskImage.resize(size);
    std::memcpy(m_impl->i10e.ramdiskImage.data(), data, size);
    m_impl->i10e.hdrRamdiskSize = size;
}

////////////////////////////////////////////////////////////////////////////////
// Second bootloader image
////////////////////////////////////////////////////////////////////////////////

/*!
 * \brief Second bootloader image
 *
 * \return Vector containing the second bootloader image binary data
 */
const std::vector<unsigned char> & BootImage::secondBootloaderImage() const
{
    return m_impl->i10e.secondImage;
}

/*!
 * \brief Set the second bootloader image
 *
 * This will automatically update the second bootloader size in the boot image
 * header and recalculate the SHA1 hash.
 */
void BootImage::setSecondBootloaderImage(std::vector<unsigned char> data)
{
    m_impl->i10e.hdrSecondSize = data.size();
    m_impl->i10e.secondImage = std::move(data);
}

void BootImage::secondBootloaderImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.secondImage.data();
    *size = m_impl->i10e.secondImage.size();
}

void BootImage::setSecondBootloaderImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.secondImage.clear();
    m_impl->i10e.secondImage.shrink_to_fit();
    m_impl->i10e.secondImage.resize(size);
    std::memcpy(m_impl->i10e.secondImage.data(), data, size);
    m_impl->i10e.hdrSecondSize = size;
}

////////////////////////////////////////////////////////////////////////////////
// Device tree image
////////////////////////////////////////////////////////////////////////////////

/*!
 * \brief Device tree image
 *
 * \return Vector containing the device tree image binary data
 */
const std::vector<unsigned char> & BootImage::deviceTreeImage() const
{
    return m_impl->i10e.dtImage;
}

/*!
 * \brief Set the device tree image
 *
 * This will automatically update the device tree size in the boot image
 * header and recalculate the SHA1 hash.
 */
void BootImage::setDeviceTreeImage(std::vector<unsigned char> data)
{
    m_impl->i10e.hdrDtSize = data.size();
    m_impl->i10e.dtImage = std::move(data);
}

void BootImage::deviceTreeImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.dtImage.data();
    *size = m_impl->i10e.dtImage.size();
}

void BootImage::setDeviceTreeImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.dtImage.clear();
    m_impl->i10e.dtImage.shrink_to_fit();
    m_impl->i10e.dtImage.resize(size);
    std::memcpy(m_impl->i10e.dtImage.data(), data, size);
    m_impl->i10e.hdrDtSize = size;
}

////////////////////////////////////////////////////////////////////////////////
// Aboot image
////////////////////////////////////////////////////////////////////////////////

const std::vector<unsigned char> & BootImage::abootImage() const
{
    return m_impl->i10e.abootImage;
}

void BootImage::setAbootImage(std::vector<unsigned char> data)
{
    m_impl->i10e.abootImage = std::move(data);
}

void BootImage::abootImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.abootImage.data();
    *size = m_impl->i10e.abootImage.size();
}

void BootImage::setAbootImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.abootImage.clear();
    m_impl->i10e.abootImage.shrink_to_fit();
    m_impl->i10e.abootImage.resize(size);
    std::memcpy(m_impl->i10e.abootImage.data(), data, size);
}

////////////////////////////////////////////////////////////////////////////////
// Sony ipl image
////////////////////////////////////////////////////////////////////////////////

const std::vector<unsigned char> & BootImage::iplImage() const
{
    return m_impl->i10e.iplImage;
}

void BootImage::setIplImage(std::vector<unsigned char> data)
{
    m_impl->i10e.iplImage = std::move(data);
}

void BootImage::iplImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.iplImage.data();
    *size = m_impl->i10e.iplImage.size();
}

void BootImage::setIplImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.iplImage.clear();
    m_impl->i10e.iplImage.shrink_to_fit();
    m_impl->i10e.iplImage.resize(size);
    std::memcpy(m_impl->i10e.iplImage.data(), data, size);
}

////////////////////////////////////////////////////////////////////////////////
// Sony rpm image
////////////////////////////////////////////////////////////////////////////////

const std::vector<unsigned char> & BootImage::rpmImage() const
{
    return m_impl->i10e.rpmImage;
}

void BootImage::setRpmImage(std::vector<unsigned char> data)
{
    m_impl->i10e.rpmImage = std::move(data);
}

void BootImage::rpmImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.rpmImage.data();
    *size = m_impl->i10e.rpmImage.size();
}

void BootImage::setRpmImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.rpmImage.clear();
    m_impl->i10e.rpmImage.shrink_to_fit();
    m_impl->i10e.rpmImage.resize(size);
    std::memcpy(m_impl->i10e.rpmImage.data(), data, size);
}

////////////////////////////////////////////////////////////////////////////////
// Sony appsbl image
////////////////////////////////////////////////////////////////////////////////

const std::vector<unsigned char> & BootImage::appsblImage() const
{
    return m_impl->i10e.appsblImage;
}

void BootImage::setAppsblImage(std::vector<unsigned char> data)
{
    m_impl->i10e.appsblImage = std::move(data);
}

void BootImage::appsblImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.appsblImage.data();
    *size = m_impl->i10e.appsblImage.size();
}

void BootImage::setAppsblImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.appsblImage.clear();
    m_impl->i10e.appsblImage.shrink_to_fit();
    m_impl->i10e.appsblImage.resize(size);
    std::memcpy(m_impl->i10e.appsblImage.data(), data, size);
}

////////////////////////////////////////////////////////////////////////////////
// Sony SIN! image
////////////////////////////////////////////////////////////////////////////////

const std::vector<unsigned char> & BootImage::sinImage() const
{
    return m_impl->i10e.sonySinImage;
}

void BootImage::setSinImage(std::vector<unsigned char> data)
{
    m_impl->i10e.sonySinImage = std::move(data);
}

void BootImage::sinImageC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.sonySinImage.data();
    *size = m_impl->i10e.sonySinImage.size();
}

void BootImage::setSinImageC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.sonySinImage.clear();
    m_impl->i10e.sonySinImage.shrink_to_fit();
    m_impl->i10e.sonySinImage.resize(size);
    std::memcpy(m_impl->i10e.sonySinImage.data(), data, size);
}

////////////////////////////////////////////////////////////////////////////////
// Sony SIN! header
////////////////////////////////////////////////////////////////////////////////

const std::vector<unsigned char> & BootImage::sinHeader() const
{
    return m_impl->i10e.sonySinHdr;
}

void BootImage::setSinHeader(std::vector<unsigned char> data)
{
    m_impl->i10e.sonySinHdr = std::move(data);
}

void BootImage::sinHeaderC(const unsigned char **data, std::size_t *size) const
{
    *data = m_impl->i10e.sonySinHdr.data();
    *size = m_impl->i10e.sonySinHdr.size();
}

void BootImage::setSinHeaderC(const unsigned char *data, std::size_t size)
{
    m_impl->i10e.sonySinHdr.clear();
    m_impl->i10e.sonySinHdr.shrink_to_fit();
    m_impl->i10e.sonySinHdr.resize(size);
    std::memcpy(m_impl->i10e.sonySinHdr.data(), data, size);
}

////////////////////////////////////////////////////////////////////////////////

bool BootImage::operator==(const BootImage &other) const
{
    // Check that the images, addresses, and metadata are equal. This doesn't
    // care if eg. one boot image is loki'd and the other is not as long as the
    // contents are the same.
    return
            // Images
            m_impl->i10e.kernelImage == other.m_impl->i10e.kernelImage
            && m_impl->i10e.ramdiskImage == other.m_impl->i10e.ramdiskImage
            && m_impl->i10e.secondImage == other.m_impl->i10e.secondImage
            && m_impl->i10e.dtImage == other.m_impl->i10e.dtImage
            && m_impl->i10e.abootImage == other.m_impl->i10e.abootImage
            // Sony images
            && m_impl->i10e.iplImage == other.m_impl->i10e.iplImage
            && m_impl->i10e.rpmImage == other.m_impl->i10e.rpmImage
            && m_impl->i10e.appsblImage == other.m_impl->i10e.appsblImage
            && m_impl->i10e.sonySinImage == other.m_impl->i10e.sonySinImage
            && m_impl->i10e.sonySinHdr == other.m_impl->i10e.sonySinHdr
            // Header's integral values
            && m_impl->i10e.hdrKernelSize == other.m_impl->i10e.hdrKernelSize
            && m_impl->i10e.kernelAddr == other.m_impl->i10e.kernelAddr
            && m_impl->i10e.hdrRamdiskSize == other.m_impl->i10e.hdrRamdiskSize
            && m_impl->i10e.ramdiskAddr == other.m_impl->i10e.ramdiskAddr
            && m_impl->i10e.hdrSecondSize == other.m_impl->i10e.hdrSecondSize
            && m_impl->i10e.secondAddr == other.m_impl->i10e.secondAddr
            && m_impl->i10e.tagsAddr == other.m_impl->i10e.tagsAddr
            && m_impl->i10e.pageSize == other.m_impl->i10e.pageSize
            && m_impl->i10e.hdrDtSize == other.m_impl->i10e.hdrDtSize
            //&& m_impl->i10e.hdrUnused == other.m_impl->i10e.hdrUnused
            // ID
            && m_impl->i10e.hdrId[0] == other.m_impl->i10e.hdrId[0]
            && m_impl->i10e.hdrId[1] == other.m_impl->i10e.hdrId[1]
            && m_impl->i10e.hdrId[2] == other.m_impl->i10e.hdrId[2]
            && m_impl->i10e.hdrId[3] == other.m_impl->i10e.hdrId[3]
            && m_impl->i10e.hdrId[4] == other.m_impl->i10e.hdrId[4]
            && m_impl->i10e.hdrId[5] == other.m_impl->i10e.hdrId[5]
            && m_impl->i10e.hdrId[6] == other.m_impl->i10e.hdrId[6]
            && m_impl->i10e.hdrId[7] == other.m_impl->i10e.hdrId[7]
            // Header's string values
            && boardName() == other.boardName()
            && kernelCmdline() == other.kernelCmdline();
}

bool BootImage::operator!=(const BootImage &other) const
{
    return !(*this == other);
}

}
