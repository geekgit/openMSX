#ifndef SCALEROUTPUT_HH
#define SCALEROUTPUT_HH

#include <concepts>
#include <span>

namespace openmsx {

template<std::unsigned_integral Pixel> class ScalerOutput
{
public:
	virtual ~ScalerOutput() = default;

	[[nodiscard]] virtual unsigned getWidth()  const = 0;
	[[nodiscard]] virtual unsigned getHeight() const = 0;

	[[nodiscard]] virtual std::span<Pixel> acquireLine(unsigned y) = 0;
	virtual void releaseLine(unsigned y, std::span<Pixel> buf) = 0;
	virtual void fillLine   (unsigned y, Pixel color) = 0;
	// TODO add copyLine() optimization

protected:
	ScalerOutput() = default;
};

} // namespace openmsx

#endif
