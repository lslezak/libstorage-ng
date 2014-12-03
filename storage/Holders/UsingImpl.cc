

#include "storage/Holders/UsingImpl.h"


namespace storage
{

    Using::Impl::Impl(const xmlNode* node)
	: Holder::Impl(node)
    {
    }


    void
    Using::Impl::save(xmlNode* node) const
    {
	Holder::Impl::save(node);
    }

}
