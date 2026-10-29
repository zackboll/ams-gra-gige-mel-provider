//===============================================================================
/// @file  OpticalDistortionMap.h
/// @brief This file includes the OpticalDistortionMap class

#pragma once
#include <chrono>
#include <cstdint>
#include <vector>
#include "IR_Directional.h"

namespace ams::iface::irmel
{
	/// @class OpticalDistortionMap
	/// @brief Optical distortion map (ODM) definition.  The ODM is defined in terms of direction cosine
	/// vectors. The coordinate frame is the same as that provided by the line of sight data such that a
	/// pixel with a forward/right/down (FRD) value of [1,0,0] points in the direction defined by the line
	/// of sight quaternion. The ODM shall be defined from edge to edge (corner to corner) of the focal plane.
	/// @Optional This class provides supporting data for image processing performed by a service
	/// and is optional in all IR MEL implementations for IR MFAs that support Optical Distortion Maps
	class OpticalDistortionMap
	{
	public:
		OpticalDistortionMap() = default;
		~OpticalDistortionMap() = default;
		OpticalDistortionMap(const OpticalDistortionMap&) = default;
		OpticalDistortionMap(OpticalDistortionMap&&) = default;
		OpticalDistortionMap& operator=(const OpticalDistortionMap&) = default;
		OpticalDistortionMap& operator=(OpticalDistortionMap&&) = default;

		[[nodiscard]] std::uint32_t getWidth() const
		{
			return this->width;
		}
		void setWidth(std::uint32_t width_in)
		{
			this->width = width_in;
		}
		[[nodiscard]] std::uint32_t getHeight() const
		{
			return this->height;
		}
		void setHeight(std::uint32_t height_in)
		{
			this->height = height_in;
		}
		[[nodiscard]] std::uint32_t getRightSize() const
		{
			return this->rightSize;
		}
		void setRightSize(std::uint32_t rightSize_in)
		{
			this->rightSize = rightSize_in;
		}
		[[nodiscard]] std::uint32_t getDownSize() const
		{
			return this->downSize;
		}
		void setDownSize(std::uint32_t downSize_in)
		{
			this->downSize = downSize_in;
		}
		[[nodiscard]] std::uint32_t getPixelsPerOdm() const
		{
			return this->pixelsPerOdm;
		}
		void setPixelsPerOdm(std::uint32_t pixelsPerOdm_in)
		{
			this->pixelsPerOdm = pixelsPerOdm_in;
		}
		[[nodiscard]] double getRightMin() const
		{
			return this->rightMin;
		}
		void setRightMin(double rightMin_in)
		{
			this->rightMin = rightMin_in;
		}
		[[nodiscard]] double getRightMax() const
		{
			return this->rightMax;
		}
		void setRightMax(double rightMax_in)
		{
			this->rightMax = rightMax_in;
		}
		[[nodiscard]] double getDownMin() const
		{
			return this->downMin;
		}
		void setDownMin(double downMin_in)
		{
			this->downMin = downMin_in;
		}
		[[nodiscard]] double getDownMax() const
		{
			return this->downMax;
		}
		void setDownMax(double downMax_in)
		{
			this->downMax = downMax_in;
		}
		[[nodiscard]] const std::vector<ForwardRightDown>& getFrd() const
		{
			return this->frd;
		}
		// replace the existing vector with a new vector
		void setFrd(const std::vector<ForwardRightDown>& frdown)
		{
			this->frd = frdown;
		}
		// add a new element to the vector
		void addForwardRightDown(const ForwardRightDown& frdown)
		{
			this->frd.push_back(frdown);
		}
		[[nodiscard]] const std::vector<RowCol>& getRowCol() const
		{
			return this->rowCol;
		}
		// replace the existing vector with a new vector
		void setRowCol(const std::vector<RowCol>& rc)
		{
			this->rowCol = rc;
		}
		// add a new element to the vector
		void addRowCol(const RowCol& rc)
		{
			this->rowCol.push_back(rc);
		}

	private:
		std::uint32_t width{0};			   ///< Number of samples for forward map [row/col -> angles]
		std::uint32_t height{0};		   ///< Number of samples for forward map [row/col -> angles]
		std::uint32_t rightSize{0};		   ///< Number of samples for backward map [angles -> row/col]
		std::uint32_t downSize{0};		   ///< Number of samples for backward map [angles -> row/col]
		std::uint32_t pixelsPerOdm{0};	   ///< Number of pixels per distortion map sample
		double rightMin{0};				   ///< Right direction cosine minimum extent used for linearly mapping
										   ///< right/down to row/col
		double rightMax{0};				   ///< Right direction cosine maximum extent used for linearly mapping
										   ///< right/down to row/col
		double downMin{0};				   ///< Down direction cosine minimum extent used for linearly mapping right/down to row/col
		double downMax{0};				   ///< Down direction cosine maximum extent used for linearly mapping right/down to row/col
		std::vector<ForwardRightDown> frd; ///< Array of forward/right/down values
		std::vector<RowCol> rowCol;		   ///< Array of row/column values
	};
} // namespace ams::iface::irmel
