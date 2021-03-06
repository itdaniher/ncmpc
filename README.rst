ncmpc
=====

ncmpc is a curses client for the `Music Player Daemon
<http://www.musicpd.org/>`__.


How to compile and install ncmpc
--------------------------------

See the `INSTALL <INSTALL>`__ file.


Usage
-----

ncmpc connects to a MPD running on a machine on the local network. 
By default, ncmpc  connects  to  localhost:6600.   This  can  be
changed  either  at  compile-time,  or  by  exporting  the MPD_HOST and
MPD_PORT environment variables, or by the command line options ``--host``
and ``--port``::

 ncmpc --host=musicserver --port=44000

For more information please view ncmpc's manual page.


Links
-----

- `Home page and download <http://www.musicpd.org/clients/ncmpc/>`__
- `git repository <https://github.com/MusicPlayerDaemon/ncmpc/>`__
- `Bug tracker <https://github.com/MusicPlayerDaemon/ncmpc/issues>`__
- `Forum <http://forum.musicpd.org/>`__
