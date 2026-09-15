/***************************************************************************
 * ROM Properties Page shell extension. (libromdata/tests)                 *
 * ZArchiveTest.cpp: ZArchive (.wua) reader test.                          *
 *                                                                         *
 * Copyright (c) 2026 by David Korth.                                      *
 * SPDX-License-Identifier: GPL-2.0-or-later                               *
 ***************************************************************************/

// Google Test
#include "gtest_init.hpp"

#include "disc/zarchive_structs.h"
#include "RomDataFactory.hpp"
#include "librpbase/RomData.hpp"
#include "librpbase/RomFields.hpp"
#include "librpfile/RpFile.hpp"
#include "librpbyteswap/byteswap_rp.h"
#include "rp-libfmt.h"

using namespace LibRomData;
using namespace LibRpFile;
using namespace LibRpBase;

namespace LibRomData { namespace Tests {

class ZArchiveTest : public ::testing::Test
{
protected:
	ZArchiveTest() = default;
};

/**
 * Verify structure sizes and alignments.
 */
TEST_F(ZArchiveTest, structSizes)
{
	EXPECT_EQ(16U, sizeof(ZArchive_OffsetInfo));
	EXPECT_EQ(144U, sizeof(ZArchive_Footer));
	EXPECT_EQ(40U, sizeof(ZArchive_CompressionOffsetRecord));
	EXPECT_EQ(16U, sizeof(ZArchive_FileDirectoryEntry));
}

/**
 * Test WUA creation via RomDataFactory if test file is present.
 */
TEST_F(ZArchiveTest, realWuaFactoryTest)
{
	static const char test_path[] = "/mnt/datos/Juegos/ROMs/Nintendo/Wii U/The Wind Waker HD (US).wua";
	auto file = std::make_shared<RpFile>(test_path, RpFile::FM_OPEN_READ);
	if (!file->isOpen()) {
		// File not present on this machine; test passes as skipped.
		return;
	}

	const RomDataPtr romData = RomDataFactory::create(file);
	ASSERT_TRUE(romData != nullptr);
	EXPECT_TRUE(romData->isValid());

	const RomFields *const fields = romData->fields();
	ASSERT_TRUE(fields != nullptr);
	EXPECT_GT(fields->count(), 0);

	bool hasTitle = false;
	bool hasTitleId = false;
	bool hasProductCode = false;
	const int count = fields->count();
	for (int i = 0; i < count; i++) {
		const auto *const field = fields->at(i);
		if (!field || !field->name) continue;
		if (!strcmp(field->name, "Title")) hasTitle = true;
		if (!strcmp(field->name, "Title ID")) hasTitleId = true;
		if (!strcmp(field->name, "Product Code")) hasProductCode = true;
	}
	EXPECT_TRUE(hasTitle);
	EXPECT_TRUE(hasTitleId);
	EXPECT_TRUE(hasProductCode);

	// Check image support
	EXPECT_TRUE(romData->supportedImageTypes() & RomData::IMGBF_INT_ICON);
	auto icon = romData->image(RomData::IMG_INT_ICON);
	ASSERT_TRUE(icon != nullptr);
	EXPECT_EQ(128, icon->width());
	EXPECT_EQ(128, icon->height());
}

} } // namespace LibRomData::Tests

#ifdef HAVE_SECCOMP
const unsigned int rp_gtest_syscall_set = 0;
#endif /* HAVE_SECCOMP */

/**
 * Test suite main function.
 */
extern "C" int gtest_main(int argc, TCHAR *argv[])
{
	fmt::print(stderr, FSTR("LibRomData test suite: ZArchive tests.\n\n"));
	fflush(nullptr);

	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
