from PyQt5.QtCore import QThread, pyqtSignal, QTimer
import time
import serial
import logging
from threading import Semaphore


class TestBoardWorker(QThread):
    update_upper_listbox = pyqtSignal(str)  # signal to update instruction listbox
    pause_serial = pyqtSignal()  # signal to pause serial worker thread
    resume_serial = pyqtSignal()  # signal to resume it

    def __init__(self, port, baudrate, timeout=5):
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None  # future serial connection object
        self.is_open = True
        self.is_running = True  # flag to keep the thread running
        self.is_stopped = False  # flag to stop the read loop
        self.last_command_time = time.time()
        self.test_data = None
        self.cli_running = False
        self.semaphore = Semaphore(1)

    # set up serial communication
    def serial_setup(self, port=None, baudrate=None):
        if port:
            self.port = port  # allow dynamic port change
        if baudrate:
            self.baudrate = baudrate
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)
            logging.info(f'connected to arduino port: {self.port}')
            print(f'connected to arduino port: {self.port}')
            time.sleep(1)  # make sure arduino is ready
            return True
        except serial.SerialException as e:
            logging.error(f'error: {e}')
            return False

    # serial response readout
    def run(self):
        if not self.serial_setup():
            logging.error(f'failed to connect to {self.port}')
            print(f'failed to connect to {self.port}')
            return
        logging.info('test board thread is running')
        print('test board thread is running')

        while self.is_running:
            if not self.is_stopped:
                self.semaphore.acquire()
                try:
                    if self.ser and self.ser.is_open:
                        logging.info('test worker thread is running')
                        print('test worker thread is running')
                        # read incoming serial data
                        response = self.ser.readline().decode('utf-8').strip()  # continuous readout from serial
                        if response:
                            self.show_response(response)
                except serial.SerialException as e:
                    logging.error(f'serial error: {e}')
                    print(f'serial error: {e}')
                    self.is_running = False
                finally:
                    self.semaphore.release()
            time.sleep(0.1)  # avoid excessive cpu usage
        self.stop()

    # method to stop the serial communication
    def stop(self):
        self.is_running = False  # stop the worker thread loop
        if self.ser and self.ser.is_open:
            self.ser.close()  # close the serial connection
            logging.info(f'connection to {self.port} closed now')
            print(f'connection to {self.port} closed now')
        self.quit()
        self.wait()

    '''
    def reopen_serial_port(self):
        # reopen the serial port with stored settings after closing it
        if self.ser and not self.ser.is_open:
            try:
                # reinitialize serial with the stored settings
                self.ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)
                logging.info(f'serial connection to {self.port} reopened successfully.')
                time.sleep(1)  # give time for arduino to reset
            except serial.SerialException as e:
                logging.error(f'error reopening serial port {self.port}: {str(e)}')
                return False
        return True '''

    # run the entire test file
    def run_all_tests(self, test_data, selected_t_port, filepath):
        if test_data and selected_t_port and filepath:  # take test_data & port number from main
            logging.info(f'running test with testdata filepath: {filepath}')
            test_data_filepath = filepath.rsplit('/', 1)[0]
            all_tests = [key for key in test_data.keys()]

            if self.ser and self.ser.is_open:
                self.pause_serial.emit()  # emit pause signal to serial capture worker thread to avoid conflicts
                self.ser.close()
                logging.info(f'serial connection to {self.port} closed before upload')
                self.semaphore.release()

            # iterate through each test and run it
            for test_key in all_tests:
                test = test_data.get(test_key, {})
                sketch_path = test.get('sketch', '')  # get .ino file path

                if sketch_path:  # if the sketch is available
                    sketch_filename = sketch_path.split('/')[-1]  # get ino file name
                    sketch_full_path = test_data_filepath + '/' + sketch_filename
                    print(sketch_full_path)

                    if not self.cli_running:  # run only if cli is not already running

                        self.cli_worker = CliWorker(selected_t_port, sketch_full_path, self.semaphore)  # create a cli worker
                        print('cli worker instantiated')
                        self.cli_thread = QThread()
                        print('cli thread created')
                        self.cli_worker.moveToThread(self.cli_thread)  # move cli worker to its thread
                        print('cli worker moved to its thread')

                        # connect signals and slots
                        self.cli_thread.started.connect(self.cli_worker.run)
                        print('cli thread connected to cli run')
                        self.cli_worker.finished.connect(self.on_cli_finished)
                        self.cli_worker.finished.connect(self.cli_worker.deleteLater)
                        self.cli_thread.finished.connect(self.cli_thread.deleteLater)

                        # start the cli thread
                        self.cli_thread.start()
                        self.cli_running = True

                else:
                    logging.warning('sketch path not found')

            # after running all tests, check the serial connection state
            if self.ser and not self.ser.is_open:
                if self.reopen_serial_port():  # using the function to reopen the port
                    self.ser.is_stopped = False  # set is_stopped to False before resuming
                    self.resume_serial.emit()  # emit resume signal to serial capture worker
                else:
                    logging.error('failed to reopen the serial port after running tests.')

        else:
            # handle case when no test data is found
            print('can\'t do it')

    # handle cli forker finish
    def on_cli_finished(self):
        logging.info('cli worker finished running')
        self.semaphore.acquire()
        self.cli_running = False  # reset flag when cli worker finishes
        if self.reopen_serial_port():
            logging.info('serial port reopened after cli worker finished')
        else:
            logging.error('failed to reopen serial port')

    # show serial response
    def show_response(self, response):
        if response:
            self.update_upper_listbox.emit(response)  # emit signal to update listbox
