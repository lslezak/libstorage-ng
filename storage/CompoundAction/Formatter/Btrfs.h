/*
 * Copyright (c) 2017 SUSE LLC
 *
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, contact SUSE LLC.
 *
 * To contact SUSE LLC about this file by physical or electronic mail, you may
 * find current contact information at www.suse.com.
 */


#ifndef STORAGE_FORMATTER_BTRFS_H
#define STORAGE_FORMATTER_BTRFS_H


#include "storage/CompoundAction/Formatter.h"
#include "storage/Filesystems/Btrfs.h"


namespace storage
{

    class CompoundAction::Formatter::Btrfs : public CompoundAction::Formatter
    {

    public:

	Btrfs(const CompoundAction::Impl* compound_action);

    private:

	string blk_devices_string_representation() const;

	Text text() const override;

	Text delete_text() const;
	Text create_and_mount_text() const;
	Text create_text() const;
	Text mount_text() const;
	Text unmount_text() const;

    private:

	const storage::Btrfs* btrfs;

    };

}

#endif
