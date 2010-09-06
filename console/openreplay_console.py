import wx
import os, sys
import signal
import traceback

all_processes = { }

MAX_CAPTURES = 8

def sigchld_handler(signum, frame):
    (pid, status) = os.wait( )
    if all_processes.has_key(pid):
        all_processes[pid].on_dead(status)
    else:
        print "unknown child died?"

def sigint_handler(signum, frame):
    global all_processes
    for x in all_processes.values:
        x.stop( )

    sys.exit(0)


signal.signal(signal.SIGCHLD, sigchld_handler)
signal.signal(signal.SIGINT, sigint_handler)

SHELL='/bin/sh'
MJPEG_INGEST_PATH='core/mjpeg_ingest'
SDL_GUI_PATH='core/sdl_gui'
BMDPLAYOUTD_PATH='core/bmdplayoutd'
FFMPEG_PATH='/usr/local/bin/ffmpeg'
MPLAYER_PATH='/usr/local/bin/mplayer'
SSH_PATH='/usr/bin/ssh'
SSH_IDENTITY='id_rsa.openreplay'

NOT_STARTED = 0
RUNNING = 1
EXITED_NORMALLY = 2
EXITED_WITH_SIGNAL = 3
EXITED_WITH_ERROR = 4
FORK_FAILED = 5
INTERRUPTED = 6

STATUS_DICT = {
    NOT_STARTED : "Not started",
    RUNNING : "Running",
    EXITED_NORMALLY : "Exited normally",
    EXITED_WITH_SIGNAL : "Terminated",
    EXITED_WITH_ERROR : "Exited with error",
    FORK_FAILED : "Fork failed",
    INTERRUPTED : "Awaiting termination"
}

class OpenreplayProcess(object):
    def __init__(self):
        self._pid = -1
        self._status = NOT_STARTED
        self._signal = None
        self._exitstatus = None

    def reinit(self):
        return False

    def configure(self):
        pass

    def config_report(self):
        return 'null'

    def config_complete(self):
        return True

    def reset(self):
        self._pid = -1
        self._status = NOT_STARTED
        self._signal = None
        self._exitstatus = None

    def on_dead(self, status):
        global all_processes
        del all_processes[self._pid]
        if os.WIFSIGNALED(status):
            self._status = EXITED_WITH_SIGNAL
            self._signal = os.WTERMSIG(status)
        else:
            self._exitstatus = os.WEXITSTATUS(status)
            if self._exitstatus == 0:
                self._status = EXITED_NORMALLY
            else:
                self._status = EXITED_WITH_ERROR

    def sigchld(self, signal, frame):
        pass

    def sigint(self, signal, frame):
        print "child got SIGINT: waiting for things to shut down"

    def pipeline_child(self):
        # child process must close STDIN, otherwise it gets SIGTSTP
        devnull = os.open('/dev/null', os.O_RDONLY)
        os.dup2(devnull, 0)

        signal.signal(signal.SIGCHLD, self.sigchld)
        signal.signal(signal.SIGINT, self.sigint)

        os.setpgid(0, 0)

        try:
            # set up pipeline
            pipeline = self.pipeline( )
            last_fd = devnull
            children = []

            # spawn the child processes
            for x in range(len(pipeline)):
                if x == len(pipeline) - 1:
                    new_stdout = None
                    next_fd = None
                else:
                    # make pipe for stdout
                    (next_fd, new_stdout) = os.pipe( )
                
                try:
                    child_pid = os.fork( )

                    if child_pid:
                        children.append(child_pid)
                        os.close(last_fd)
                        last_fd = next_fd
                        if new_stdout is not None:
                            os.close(new_stdout)

                    else:
                        # child process - redirect stdin and stdout then go go go
                        os.dup2(last_fd, 0)
                        if new_stdout is not None:
                            os.dup2(new_stdout, 1)
                        self.exec_with_args(pipeline[x][0], pipeline[x][1])

                except OSError:
                    raise


            status = 0
            while len(children) > 0:
                try:
                    (pid, status) = os.wait( )
                    children.remove(pid)
                except OSError, e:
                    if e.errno == 4:
                        # interrupted system call - just try again...
                        pass
                    else:
                        # something dire happened so re-raise it
                        raise

            os._exit(status) # not 100% perfect...
        except:
            traceback.print_exc( )
            # don't forget to kill the children!
            os.kill(0, signal.SIGTERM)
            os._exit(1)

    def exec_with_args(self, cmd, args):
        os.execl(cmd, cmd, *args)

    def start(self):        
        global all_processes

        self._status = RUNNING
        try:
            self._pid = os.fork( )
        except OSError:
            print "fork failed"
            self._status = FORK_FAILED
            return

        if self._pid == 0:
            try:
                self.pipeline_child( )
            except:
                traceback.print_exc( )
                os._exit(1)
        else:
            # parent process (make the child process a group leader)
            print "parent process: setting process group ID"
            os.setpgid(self._pid, 0)
            print "parent: done!"
            all_processes[self._pid] = self


    def pipeline(self):
        # should return a list of tuples in the form (program, [ args ] ).
        # Each program's output will be piped into the next program's input.
        return []

    def status(self):
        return self._status

    def stop(self):
        if self._status == RUNNING:
            os.kill(-self._pid, signal.SIGINT)
            self._status = INTERRUPTED
        elif self._status == INTERRUPTED:
            # KILL DASH NINE! No more CPU time!
            # Kill dash nine and your process(-group) is mine!
            os.kill(-self._pid, signal.SIGKILL)
        else:
            print "tried to stop something that wasn't running?"

    @classmethod
    def name(cls):
        return 'null'

class CaptureProcess(OpenreplayProcess):
    def __init__(self):
        super(CaptureProcess, self).__init__( )
        self._buf_file = None
    
    def get_buffer(self):
        return self._buf_file

    def set_buffer(self, buf_file):
        self._buf_file = buf_file

    def choose_buffer(self, parent_win=None):
        dlg = wx.FileDialog(parent_win, 'Select Buffer File', style=wx.FD_OPEN)
        if dlg.ShowModal() == wx.ID_OK:
            self._buf_file = dlg.GetPath( )
        else:
            print "buffer selection cancelled by user"

    def config_complete(self):
        if self._buf_file is not None:
            return True
        else:
            return False

    def config_report(self):
        if self.config_complete( ):
            return ["buffer file: " + self._buf_file]
        else:
            return ["Configuration incomplete"]

class LocalFileCaptureProcess(CaptureProcess):
    def __init__(self):
        super(LocalFileCaptureProcess, self).__init__( )
        self._filename = None

    def configure(self, parent_win = None):
        dlg = wx.FileDialog(parent_win, 'Select Video File', style=wx.FD_OPEN)
        if dlg.ShowModal( ) == wx.ID_OK:
            self._filename = dlg.GetPath( )
            self.choose_buffer( )
        else:
            print "input file selection cancelled by user"

    def config_complete(self):
        if super(LocalFileCaptureProcess, self).config_complete( ):
            if self._filename is not None:
                return True

        return False


    def pipeline(self):
        if self.config_complete( ):
            return [
                (FFMPEG_PATH, ["-i", self._filename, "-f", "mjpeg", "-qscale", "5", "-s", "720x480", "-"]),
                (MJPEG_INGEST_PATH, [ self.get_buffer( ) ])
            ]
        else:
            return [] 

    def config_report(self):
        if self.config_complete( ):
            return [
                "Local file capture from " + self._filename
            ] + super(LocalFileCaptureProcess, self).config_report( )
        else:
            return ["Configuration incomplete"]

    @classmethod
    def name(cls):
        return 'Capture from local file'

class SSHFileCaptureProcess(CaptureProcess):
    def __init__(self):
        super(SSHFileCaptureProcess, self).__init__( )
        self._filename = None
        self._hostname = None

    def configure(self, parent_win = None):
        dlg1 = wx.TextEntryDialog(parent_win, 
            'Please enter the SSH username and server (i.e. armena@127.0.0.1)', 'Remote Capture Configuration',
            'armena@127.0.0.1'
        )
        if dlg1.ShowModal( ) == wx.ID_OK:
            new_hostname = dlg1.GetValue( )
        else:
            print "account/hostname configuration cancelled by user"
            return

        dlg2 = wx.TextEntryDialog(parent_win,
            'Please enter the filename to encode from', 'Remote Capture Configuration',
            'fail.mov'
        )
        if dlg2.ShowModal( ) == wx.ID_OK:
            self._filename = dlg2.GetValue( )
            self._hostname = new_hostname
        else:
            print "filename configuration cancelled by user"
            return

        self.choose_buffer( )

    def config_complete(self):
        if super(SSHFileCaptureProcess, self).config_complete( ):
            if self._filename is not None:
                return True

        return False


    def pipeline(self):
        if self.config_complete( ):
            return [
                # SSH to remote box and start ffmpeg
                (SSH_PATH, ["-i", SSH_IDENTITY, self._hostname, "ffmpeg", "-i", self._filename, "-f", "mjpeg", "-qscale", "5", "-s", "720x480", "-"]),
                # pipe into mjpeg_ingest
                (MJPEG_INGEST_PATH, [ self.get_buffer( ) ])
            ]
        else:
            return [] 

    def config_report(self):
        if self.config_complete( ):
            return [
                "Local file capture from " + self._filename
            ] + super(SSHFileCaptureProcess, self).config_report( )
        else:
            return ["Configuration incomplete"]

    @classmethod
    def name(cls):
        return 'Capture from remote file via SSH'

class ConsumerProcess(OpenreplayProcess):
    def __init__(self):
        super(ConsumerProcess, self).__init__( )
        self._capture_processes = None

    def set_capture_processes(self, processes):
        self._capture_processes = processes

    def config_complete(self):
        if self._capture_processes is not None and len(self._capture_processes) > 0:
            if self.capture_processes_configs_complete( ):
                return True
            else:
                return False
        else:
            return False

    def config_report(self):
        return []

    def capture_processes_configs_complete(self):
        flag = True
        for x in self._capture_processes:
            if not x.config_complete( ):
                flag = False

        return flag

    
class SDLGUIProcess(ConsumerProcess):
    def pipeline(self):
        return [ (SDL_GUI_PATH, [ x.get_buffer( ) for x in self._capture_processes ] ) ]

    @classmethod
    def name(cls):
        return 'SDL GUI'

class MplayerPlayoutProcess(ConsumerProcess):
    def pipeline(self):
        return [ 
            (BMDPLAYOUTD_PATH, [ x.get_buffer( ) for x in self._capture_processes ] ), 
            (MPLAYER_PATH, ['-vo', 'xv', '-demuxer', 'rawvideo', '-rawvideo', 'uyvy:ntsc', '-'])
        ]

    @classmethod
    def name(cls):
        return 'Stdout Playout Daemon to MPlayer'


INPUTS = [ LocalFileCaptureProcess, SSHFileCaptureProcess ]
GUIS = [ SDLGUIProcess ] 
PLAYOUTS = [ MplayerPlayoutProcess ]


class ClassChooser(wx.ComboBox):
    def __init__(self, parent, classes, allow_none=False):
        assert len(classes) > 0

        self._classes = classes
        self._names = [ klass.name( ) for klass in classes ]

        if allow_none:
            self._names = [ 'None' ] + self._names
            self._classes = [ None ] + self._classes
        
        wx.ComboBox.__init__(self, parent, -1, choices=self._names)

        self.SetSelection(0)

    def GetClass(self):
        return self._classes[self.GetSelection( )]
        

class StatusWidget(wx.StaticText):
    def __init__(self, parent):
        wx.StaticText.__init__(self, parent, -1)

    def SetStatus(self, stat_code):
        global STATUS_DICT
        self.SetLabel(STATUS_DICT[stat_code])

    def ClearStatus(self):
        self.SetLabel('')

ALLOW_NONE=True
REQUIRE_SOMETHING=False

class ProcessPanel(wx.Panel):
    def __init__(self, parent, classes, allow_none=False):
        wx.Panel.__init__(self, parent, -1)
        self._process = None
        self._notify = None
        
        self.status_widget = StatusWidget(self)
        self.class_chooser = ClassChooser(self, classes, allow_none)
        self.start_button = wx.Button(self, -1, 'Start')
        self.stop_button = wx.Button(self, -1, 'Kill')
        self.configure_button = wx.Button(self, -1, 'Configure')

        self.Bind(wx.EVT_BUTTON, self.OnStart, self.start_button)
        self.Bind(wx.EVT_BUTTON, self.OnStop, self.stop_button)
        self.Bind(wx.EVT_BUTTON, self.OnConfigure, self.configure_button)
        self.Bind(wx.EVT_COMBOBOX, self.OnChangeClass, self.class_chooser)

        sz = wx.BoxSizer(wx.HORIZONTAL)
        sz.Add(self.status_widget, 1, wx.ALIGN_CENTER | wx.ALL, 1)
        sz.Add(self.class_chooser, 1, wx.ALIGN_CENTER | wx.ALL, 1)
        sz.Add(self.start_button, 0, wx.ALIGN_CENTER | wx.ALL, 1)
        sz.Add(self.stop_button, 0, wx.ALIGN_CENTER | wx.ALL, 1)
        sz.Add(self.configure_button, 0, wx.ALIGN_CENTER | wx.ALL, 1)

        self.SetSizer(sz)
        self.SetAutoLayout(1)

        self.update_class( )


    def OnStart(self, event):
        if self._process is not None:
            self._process.start( )
            self.poll( )

    def OnStop(self, event):
        if self._process is not None:
            self._process.stop( )
            self.poll( )

    def OnConfigure(self, event):
        if self._process is not None:
            self._process.configure(self)
            self.poll( )

    def OnChangeClass(self, event):
        self.update_class( )

    def poll(self):
        if self._process is not None:
            status = self._process.status( )
            self.status_widget.SetStatus(status)
            if not self._process.config_complete( ):
                self.class_chooser.Enable( )
                self.configure_button.Enable( )
                self.start_button.Disable( )
                self.stop_button.Disable( )
            elif status == RUNNING or status == INTERRUPTED:
                # It's running, or we're waiting for it to stop.
                self.stop_button.Enable( )
                self.start_button.Disable( )
                self.class_chooser.Disable( )
                self.configure_button.Disable( )
            else:
                # it's not running, or stopped for some reason or other
                self.start_button.Enable( )
                self.configure_button.Enable( )
                self.class_chooser.Enable( )
                self.stop_button.Disable( )

        else:
            self.status_widget.ClearStatus( )
            self.start_button.Disable( )
            self.stop_button.Disable( )
            self.configure_button.Disable( )
            self.class_chooser.Enable( )

    def update_class(self):
        klass = self.class_chooser.GetClass( )
        if klass is None:
            self._process = None
        else:
            self._process = klass( )

        if self._notify is not None:
            self._notify(self)

        self.poll( )

            
    def get_process(self):
        return self._process

    def register_notify(self,fn):
        self._notify = fn


class MainFrame(wx.Frame):
    def __init__(self):          
        wx.Frame.__init__(self, None, -1, 'Openreplay Console')

        sz = wx.BoxSizer(wx.VERTICAL)

        # construct input panels
        sbox = wx.StaticBox(self, -1, 'Video Capture')
        sz1 = wx.StaticBoxSizer(sbox, wx.VERTICAL)
        self.input_process_panels = []
        for x in range(MAX_CAPTURES):
            pp = ProcessPanel(self, INPUTS, ALLOW_NONE);
            pp.register_notify(self.inputs_changed)
            sz1.Add(pp, 0, wx.EXPAND | wx.ALL, 1)
            self.input_process_panels.append(pp)

        sz.Add(sz1, 0, wx.EXPAND | wx.ALL, 1)

        # GUI
        sbox = wx.StaticBox(self, -1, 'Control Interface')
        sz1 = wx.StaticBoxSizer(sbox, wx.VERTICAL)
        self.gui_panel = ProcessPanel(self, GUIS, REQUIRE_SOMETHING)
        sz1.Add(self.gui_panel, 0, wx.EXPAND | wx.ALL, 1)
        sz.Add(sz1, 0, wx.EXPAND | wx.ALL, 1)

        # playout
        sbox = wx.StaticBox(self, -1, 'Playout')
        sz1 = wx.StaticBoxSizer(sbox, wx.VERTICAL)
        self.playout_panel = ProcessPanel(self, PLAYOUTS, REQUIRE_SOMETHING)
        sz1.Add(self.playout_panel, 0, wx.EXPAND | wx.ALL, 1)
        sz.Add(sz1, 0, wx.EXPAND | wx.ALL, 1)
        
        self.SetSizer(sz)
        self.Fit( )
        
        # construct a timer for the periodic polling
        self.timer = wx.PyTimer(self.poll_children)
        self.timer.Start(1000)
        self.poll_children( )

    def poll_children(self):
        for x in self.input_process_panels:
            x.poll( )
        self.gui_panel.poll( )
        self.playout_panel.poll( )

    def inputs_changed(self, in_panel):
        args = []

        gui = self.gui_panel.get_process( )
        playout = self.playout_panel.get_process( )
        
        for panel in self.input_process_panels:
            process = panel.get_process( )
            if process is not None:
                args.append(process)

        if gui is not None:
            gui.set_capture_processes(args)
            self.gui_panel.poll( )

        if playout is not None:
            playout.set_capture_processes(args)
            self.playout_panel.poll( )

class OpenreplayConsoleApp(wx.App):
    def OnInit(self):
        frame = MainFrame()    
        frame.Show(True)
        self.SetTopWindow(frame)
        return True

app = OpenreplayConsoleApp(0)
app.MainLoop( )
