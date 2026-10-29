#pragma once
#include <chrono>
#include <variant>
#include <vector>
#include "BaseSecurityEventType.h"

namespace ams::iface::mel
{
	/// @class SecurityArtifact
	/// @brief A class that holds an artifact UUID entry associated with a component UUID
	/// @Optional This class defines data that can be used in publishing Security Audit Records and is an optional component
	class SecurityArtifact
	{
	public:
		SecurityArtifact() = default;
		SecurityArtifact(const UCI_ID& componentID_in, const UCI_ID& associatedID_in) : componentID{componentID_in}, associatedID{associatedID_in}
		{
		}
		~SecurityArtifact() = default;
		SecurityArtifact(const SecurityArtifact&) = default;
		SecurityArtifact(SecurityArtifact&&) = default;
		SecurityArtifact& operator=(const SecurityArtifact&) = default;
		SecurityArtifact& operator=(SecurityArtifact&&) = default;

		[[nodiscard]] const UCI_ID& getComponentID() const
		{
			return componentID;
		}
		void setComponentID(const UCI_ID& newValue)
		{
			this->componentID = newValue;
		}
		[[nodiscard]] const UCI_ID& getAssociatedID() const
		{
			return associatedID;
		}
		void setAssociatedID(const UCI_ID& newValue)
		{
			this->associatedID = newValue;
		}

	private:
		/// UUID for a specific component
		UCI_ID componentID;
		/// UUID of associated item, if needed (e.g. image file component was unable to authenticate)
		UCI_ID associatedID;
	};
} // end namespace ams::iface::mel
