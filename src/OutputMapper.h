/**
 * The output mapper builds a relation between output groups (or a collection of
 * groups, called an ubergroup) and an effect routine.
 */
#ifndef OUTPUTMAPPER_H
#define OUTPUTMAPPER_H

#include <map>
#include <set>
#include <vector>

#include "DataStore.h"
#include "Framebuffer.h"

#include "Routine.h"

class OutputMapper {
	public:
		OutputMapper(DataStore *s, Framebuffer *f);
		~OutputMapper();

	public:
		class OutputGroup {
			friend class OutputMapper;

			public:
				OutputGroup() = delete;
				OutputGroup(DataStore::Group *g);
				virtual ~OutputGroup() {};

				/**
				 * Returns an iterator into the group's framebuffer.
				 */
				std::vector<HSIPixel>::iterator getDataPointer() {
					return this->buffer.begin();
				}

				/**
				 * Returns the number of pixels in the group.
				 */
				int numPixels() const {
					return this->group->numPixels();
				}

			private:
				DataStore::Group *group = nullptr;

				std::vector<HSIPixel> buffer;

				friend bool operator==(const OutputGroup& lhs, const OutputGroup& rhs);
				friend bool operator< (const OutputGroup& lhs, const OutputGroup& rhs);
		};

		class OutputUberGroup: public OutputGroup {
			friend class OutputMapper;

			public:
				OutputUberGroup();
				OutputUberGroup(std::vector<OutputGroup *> &members);
				~OutputUberGroup() {};

			private:
				void addMember(OutputGroup *group);
				void removeMember(OutputGroup *group);
				bool containsMember(OutputGroup *group);

			private:
				void _resizeBuffer();

			private:
				std::set<OutputGroup *> groups;
		};

	public:
		void addMapping(OutputGroup *g, Routine *r);
		void removeMappingForGroup(OutputGroup *g);

	private:
		void _removeMappingsInUbergroup(OutputUberGroup *ug);

	private:
		DataStore *store;
		Framebuffer *fb;

		std::map<OutputGroup *, Routine *> outputMap;
};

#endif