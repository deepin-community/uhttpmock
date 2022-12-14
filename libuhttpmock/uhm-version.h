/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * uhttpmock
 * Copyright (C) Holger Berndt 2011 <hb@gnome.org>
 * Copyright (C) Philip Withnall 2013 <philip@tecnocode.co.uk>
 *
 * uhttpmock is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * uhttpmock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with uhttpmock.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UHM_VERSION_H
#define UHM_VERSION_H

/**
 * SECTION:uhm-version
 * @Short_description: Macros to check the libuhttpmock version
 * @Title: Version Information
 *
 * uhttpmock provides compile-time version information.
 *
 * Since: 0.1.0
 */

/**
 * UHM_MAJOR_VERSION:
 *
 * Evaluates to the major version of the libuhttpmock headers at compile time.
 * (e.g. in libuhttpmock version 1.2.3 this is 1).
 *
 * Since: 0.1.0
 */
#define UHM_MAJOR_VERSION (0)

/**
 * UHM_MINOR_VERSION:
 *
 * Evaluates to the minor version of the libuhttpmock headers at compile time.
 * (e.g. in libuhttpmock version 1.2.3 this is 2).
 *
 * Since: 0.1.0
 */
#define UHM_MINOR_VERSION (5)

/**
 * UHM_MICRO_VERSION:
 *
 * Evaluates to the micro version of the libuhttpmock headers at compile time.
 * (e.g. in libuhttpmock version 1.2.3 this is 3).
 *
 * Since: 0.1.0
 */
#define UHM_MICRO_VERSION (3)

/**
 * UHM_CHECK_VERSION:
 * @major: major version (e.g. 1 for version 1.2.3)
 * @minor: minor version (e.g. 2 for version 1.2.3)
 * @micro: micro version (e.g. 3 for version 1.2.3)
 *
 * Evaluates to %TRUE if the version of the libuhttpmock header files
 * is the same as or newer than the passed-in version.
 *
 * Since: 0.1.0
 */
#define UHM_CHECK_VERSION(major,minor,micro) \
    (UHM_MAJOR_VERSION > (major) || \
     (UHM_MAJOR_VERSION == (major) && UHM_MINOR_VERSION > (minor)) || \
     (UHM_MAJOR_VERSION == (major) && UHM_MINOR_VERSION == (minor) && \
      UHM_MICRO_VERSION >= (micro)))

#endif /* !UHM_VERSION_H */
