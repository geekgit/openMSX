#include "V9990SDLRasterizer.hh"
#include "V9990.hh"
#include "RawFrame.hh"
#include "PostProcessor.hh"
#include "Display.hh"
#include "OutputSurface.hh"
#include "RenderSettings.hh"
#include "MemoryOps.hh"
#include "enumerate.hh"
#include "narrow.hh"
#include "one_of.hh"
#include "xrange.hh"
#include "build-info.hh"
#include "components.hh"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

namespace openmsx {

template<std::unsigned_integral Pixel>
V9990SDLRasterizer<Pixel>::V9990SDLRasterizer(
		V9990& vdp_, Display& display, OutputSurface& screen_,
		std::unique_ptr<PostProcessor> postProcessor_)
	: vdp(vdp_), vram(vdp.getVRAM())
	, screen(screen_)
	, workFrame(std::make_unique<RawFrame>(screen.getPixelFormat(), 1280, 240))
	, renderSettings(display.getRenderSettings())
	, postProcessor(std::move(postProcessor_))
	, bitmapConverter(vdp, palette64, palette64_32768, palette256, palette256_32768, palette32768)
	, p1Converter(vdp, palette64)
	, p2Converter(vdp, palette64)
{
	// Fill palettes
	preCalcPalettes();

	renderSettings.getGammaSetting()      .attach(*this);
	renderSettings.getBrightnessSetting() .attach(*this);
	renderSettings.getContrastSetting()   .attach(*this);
	renderSettings.getColorMatrixSetting().attach(*this);
}

template<std::unsigned_integral Pixel>
V9990SDLRasterizer<Pixel>::~V9990SDLRasterizer()
{
	renderSettings.getColorMatrixSetting().detach(*this);
	renderSettings.getGammaSetting()      .detach(*this);
	renderSettings.getBrightnessSetting() .detach(*this);
	renderSettings.getContrastSetting()   .detach(*this);
}

template<std::unsigned_integral Pixel>
PostProcessor* V9990SDLRasterizer<Pixel>::getPostProcessor() const
{
	return postProcessor.get();
}

template<std::unsigned_integral Pixel>
bool V9990SDLRasterizer<Pixel>::isActive()
{
	return postProcessor->needRender() &&
	       vdp.getMotherBoard().isActive() &&
	       !vdp.getMotherBoard().isFastForwarding();
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::reset()
{
	setDisplayMode(vdp.getDisplayMode());
	setColorMode(vdp.getColorMode());
	resetPalette();
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::frameStart()
{
	const V9990DisplayPeriod& horTiming = vdp.getHorizontalTiming();
	const V9990DisplayPeriod& verTiming = vdp.getVerticalTiming();

	// Center image on the window.

	// In SDLLo, one window pixel represents 8 UC clock ticks, so the
	// window = 320 * 8 UC ticks wide. In SDLHi, one pixel is 4 clock-
	// ticks and the window 640 pixels wide -- same amount of UC ticks.
	colZero = horTiming.blank + horTiming.border1 +
	          (horTiming.display - SCREEN_WIDTH * 8) / 2;

	// 240 display lines can be shown. In SDLHi, we can do interlace,
	// but still 240 lines per frame.
	lineRenderTop = verTiming.blank + verTiming.border1 +
	                (verTiming.display - SCREEN_HEIGHT) / 2;
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::frameEnd(EmuTime::param time)
{
	workFrame = postProcessor->rotateFrames(std::move(workFrame), time);
	workFrame->init(
	    vdp.isInterlaced() ? (vdp.getEvenOdd() ? RawFrame::FIELD_EVEN
	                                           : RawFrame::FIELD_ODD)
	                       : RawFrame::FIELD_NONINTERLACED);
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::setDisplayMode(V9990DisplayMode mode)
{
	displayMode = mode;
	bitmapConverter.setColorMode(colorMode, displayMode);
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::setColorMode(V9990ColorMode mode)
{
	colorMode = mode;
	bitmapConverter.setColorMode(colorMode, displayMode);
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::drawBorder(
	int fromX, int fromY, int limitX, int limitY)
{
	Pixel bgColor = vdp.isOverScan()
	              ? 0
	              : palette64[vdp.getBackDropColor() & 63];

	int startY = std::max(fromY  - lineRenderTop,   0);
	int endY   = std::min(limitY - lineRenderTop, 240);
	if (startY >= endY) return;

	if ((fromX == 0) && (limitX == V9990DisplayTiming::UC_TICKS_PER_LINE)) {
		// optimization
		for (auto y : xrange(startY, endY)) {
			workFrame->setBlank(y, bgColor);
		}
		return;
	}

	static int const screenW = SCREEN_WIDTH * 8; // in ticks
	int startX = std::max(0, V9990::UCtoX(fromX - colZero, displayMode));
	int endX = V9990::UCtoX(
		(limitX == V9990DisplayTiming::UC_TICKS_PER_LINE)
		? screenW : std::min(screenW, limitX - colZero), displayMode);
	if (startX >= endX) return;

	unsigned lineWidth = vdp.getLineWidth();
	MemoryOps::MemSet<Pixel> memset;
	for (auto y : xrange(startY, endY)) {
		memset(workFrame->getLineDirect<Pixel>(y).subspan(startX, size_t(endX - startX)),
		       bgColor);
		workFrame->setLineWidth(y, lineWidth);
	}
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::drawDisplay(
	int fromX, int fromY, int toX, int toY,
	int displayX, int displayY, int displayYA, int displayYB)
{
	static int const screenW = SCREEN_WIDTH * 8;
	static int const screenH = SCREEN_HEIGHT;

	// from VDP coordinates to screen coordinates
	fromX -= colZero;
	toX   -= colZero;
	fromY -= lineRenderTop;
	toY   -= lineRenderTop;

	// Clip to screen
	if (fromX < 0) {
		displayX -= fromX;
		fromX = 0;
	}
	if (fromY < 0) {
		displayY  -= fromY;
		displayYA -= fromY;
		displayYB -= fromY;
		fromY = 0;
	}
	if (toX > screenW) {
		toX = screenW;
	}
	if (toY > screenH) {
		toY = screenH;
	}
	fromX = V9990::UCtoX(fromX, displayMode);
	toX   = V9990::UCtoX(toX,   displayMode);

	if ((toX > fromX) && (toY > fromY)) {
		bool drawSprites = vdp.spritesEnabled() &&
			!renderSettings.getDisableSprites();

		displayX = V9990::UCtoX(displayX, displayMode);
		int displayWidth  = toX - fromX;
		int displayHeight = toY - fromY;

		if (displayMode == P1) {
			drawP1Mode(fromX, fromY, displayX,
			           displayY, displayYA, displayYB,
			           displayWidth, displayHeight,
			           drawSprites);
		} else if (displayMode == P2) {
			drawP2Mode(fromX, fromY, displayX,
			           displayY, displayYA,
			           displayWidth, displayHeight,
			           drawSprites);
		} else {
			drawBxMode(fromX, fromY, displayX,
			           displayY, displayYA,
			           displayWidth, displayHeight,
			           drawSprites);
		}
	}
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::drawP1Mode(
	int fromX, int fromY, int displayX,
	int displayY, int displayYA, int displayYB,
	int displayWidth, int displayHeight, bool drawSprites)
{
	while (displayHeight--) {
		auto dst = workFrame->getLineDirect<Pixel>(fromY).subspan(fromX, displayWidth);
		p1Converter.convertLine(dst, displayX, displayY,
		                        displayYA, displayYB, drawSprites);
		workFrame->setLineWidth(fromY, 320);
		++fromY;
		++displayY;
		++displayYA;
		++displayYB;
	}
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::drawP2Mode(
	int fromX, int fromY, int displayX, int displayY, int displayYA,
	int displayWidth, int displayHeight, bool drawSprites)
{
	while (displayHeight--) {
		auto dst = workFrame->getLineDirect<Pixel>(fromY).subspan(fromX, displayWidth);
		p2Converter.convertLine(dst, displayX, displayY, displayYA, drawSprites);
		workFrame->setLineWidth(fromY, 640);
		++fromY;
		++displayY;
		++displayYA;
	}
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::drawBxMode(
	int fromX, int fromY, int displayX, int displayY, int displayYA,
	int displayWidth, int displayHeight, bool drawSprites)
{
	unsigned scrollX = vdp.getScrollAX();
	unsigned x = displayX + scrollX;

	int lineStep = 1;
	if (vdp.isEvenOddEnabled()) {
		if (vdp.getEvenOdd()) {
			++displayY;
			++displayYA;
		}
		lineStep = 2;
	}

	unsigned scrollY = vdp.getScrollAY();
	unsigned rollMask = vdp.getRollMask(0x1FFF);
	unsigned scrollYBase = scrollY & ~rollMask & 0x1FFF;
	int cursorY = displayY - vdp.getCursorYOffset();
	while (displayHeight--) {
		// Note: convertLine() can draw up to 3 pixels too many. But
		// that's ok, the buffer is big enough: buffer can hold 1280
		// pixels, max displayWidth is 1024 pixels. When taking the
		// position of the borders into account, the display area
		// plus 3 pixels cannot go beyond the end of the buffer.
		unsigned y = scrollYBase + ((displayYA + scrollY) & rollMask);
		auto dst = workFrame->getLineDirect<Pixel>(fromY).subspan(fromX);
		bitmapConverter.convertLine(dst.subspan(displayWidth), x, y,
		                            cursorY, drawSprites);
		workFrame->setLineWidth(fromY, vdp.getLineWidth());
		++fromY;
		displayYA += lineStep;
		cursorY   += lineStep;
	}
}


template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::preCalcPalettes()
{
	// the 32768 color palette
	if (renderSettings.isColorMatrixIdentity()) {
		// Most users use the "normal" monitor type; making this a
		// special case speeds up palette precalculation a lot.
		std::array<int, 32> intensity;
		for (auto [i, r] : enumerate(intensity)) {
			r = narrow_cast<int>(255.0f * renderSettings.transformComponent(narrow<float>(i) / 31.0f));
		}
		for (auto [grb, col] : enumerate(palette32768)) {
			col = screen.mapKeyedRGB255<Pixel>(gl::ivec3(
				intensity[(grb >>  5) & 31],
				intensity[(grb >> 10) & 31],
				intensity[(grb >>  0) & 31]));
		}
	} else {
		for (auto g : xrange(32)) {
			for (auto r : xrange(32)) {
				for (auto b : xrange(32)) {
					gl::vec3 rgb{narrow<float>(r),
					             narrow<float>(g),
					             narrow<float>(b)};
					palette32768[(g << 10) + (r << 5) + b] = Pixel(
						screen.mapRGB(renderSettings.transformRGB(rgb / 31.0f)));
				}
			}
		}
	}

	// the 256 color palette
	std::array<int, 8> mapRG = {0, 4, 9, 13, 18, 22, 27, 31};
	std::array<int, 4> mapB = {0, 11, 21, 31};
	for (auto g : xrange(8)) {
		for (auto r : xrange(8)) {
			for (auto b : xrange(4)) {
				auto idx256 = (g << 5) | (r << 2) | b;
				auto idx32768 = (mapRG[g] << 10) | (mapRG[r] << 5) | mapB[b];
				palette256_32768[idx256] = narrow<int16_t>(idx32768);
				palette256[idx256] = palette32768[idx32768];
			}
		}
	}

	resetPalette();
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::setPalette(int index,
                                           byte r, byte g, byte b, bool ys)
{
	auto idx32768 = ((g & 31) << 10) | ((r & 31) << 5) | ((b & 31) << 0);
	palette64_32768[index & 63] = narrow<int16_t>(idx32768); // TODO what with ys?
	palette64[index & 63] = ys ? screen.getKeyColor<Pixel>()
	                           : palette32768[idx32768];
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::resetPalette()
{
	// get 64 color palette from VDP
	for (auto i : xrange(64)) {
		auto [r, g, b, ys] = vdp.getPalette(i);
		setPalette(i, r, g, b, ys);
	}
	palette256[0] = vdp.isSuperimposing() ? screen.getKeyColor<Pixel>()
	                                      : palette32768[0];
	// TODO what with palette256_32768[0]?
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::setSuperimpose(bool /*enabled*/)
{
	resetPalette();
}

template<std::unsigned_integral Pixel>
bool V9990SDLRasterizer<Pixel>::isRecording() const
{
	return postProcessor->isRecording();
}

template<std::unsigned_integral Pixel>
void V9990SDLRasterizer<Pixel>::update(const Setting& setting) noexcept
{
	if (&setting == one_of(&renderSettings.getGammaSetting(),
	                       &renderSettings.getBrightnessSetting(),
	                       &renderSettings.getContrastSetting(),
	                       &renderSettings.getColorMatrixSetting())) {
		preCalcPalettes();
	}
}

// Force template instantiation.
#if HAVE_16BPP
template class V9990SDLRasterizer<uint16_t>;
#endif
#if HAVE_32BPP || COMPONENT_GL
template class V9990SDLRasterizer<uint32_t>;
#endif

} // namespace openmsx
