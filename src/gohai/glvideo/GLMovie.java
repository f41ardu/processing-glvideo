/* -*- mode: java; c-basic-offset: 2; indent-tabs-mode: nil -*- */

/*
  Copyright (c) The Processing Foundation 2016
  Developed by Gottfried Haider

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General
  Public License along with this library; if not, write to the
  Free Software Foundation, Inc., 59 Temple Place, Suite 330,
  Boston, MA  02111-1307  USA
*/

package gohai.glvideo;

import java.io.File;
import processing.core.*;

/**
 *  @webref
 */
public class GLMovie extends GLVideo {

  public GLMovie(PApplet parent, String fn_or_uri) {
    this(parent, fn_or_uri, 0);
  }

  public GLMovie(PApplet parent, String fn_or_uri, int flags) {
    super(parent);

    if (fn_or_uri.indexOf("://") != -1) {
      // got URI, use as is
    } else {
      // get absolute path for fn
      // first, check Processing's dataPath
      File file = new File(parent.dataPath(fn_or_uri));
      if (file.exists() == false) {
        // next, the current directory
        file = new File(fn_or_uri);
      }
      if (file.exists()) {
        fn_or_uri = file.getAbsolutePath();
      }
    }

    handle = GLNative.gstreamer_open(fn_or_uri, flags);
    if (handle == 0) {
      throw new RuntimeException("Could not load video");
    }
  }
}
