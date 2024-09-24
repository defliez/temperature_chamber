from kivy.app import App
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.label import Label
from kivy.uix.button import Button
from kivy.uix.textinput import TextInput
import serial
import time


"""
MUST DO:
CHECK TEMP CONTINUOUSLY
EXTRACT TEMP AND SEE WHEN DESIRED TEMP IS REACHED
THEN SET OFF THE TIMER TO KEEP THE DESIRED TEMP FOR DESIRED TIME
UPDATE INFO ON DISPLAY

***

REUSE AND ADAPT THIS:

response = self.ser.readline().decode('utf-8').strip()
print(f"received: {response}")  # Debug print
self.label.text = f"{response}"
if response and not response.startswith("enter a value:"):
    data = response.split("|")
    if len(data) == 6:
        RealTemp, ThermocoupleTemp1, ThermocoupleTemp2, ThermocoupleAverage, CurrentError, CurrentErrorPercentage = data
        self.label.text = (f"RealTemp: {RealTemp}°C\n"
                            f"ThermocoupleTemp1: {ThermocoupleTemp1}°C\n"
                            f"ThermocoupleTemp2: {ThermocoupleTemp2}°C\n"
                            f"Avg Temp: {ThermocoupleAverage}°C\n"
                            f"Error: {CurrentError}°C\n"
                            f"Error %: {CurrentErrorPercentage}%")

*****

use this?
TemperatureThreshold= temperatureRoom-temperatureDesired;
as in:
if TemperatureThreshold==0:
Clock.schedule_once(partial(self.send_command, None, "SYSTEM OFF"), time_in_seconds)

need to get at the actual temperature in the chamber, have it returned as an int or a float
******

MOST DEF USE THREADING

"""
class TemperatureControlApp(App):
    def build(self):
        # Set up serial communication with Arduino
        try:
            self.ser = serial.Serial("COM13", baudrate=9600, timeout=5)
            print("connected to Arduino port: COM13")
        except serial.SerialException as e:
            print(f"Error: {e}")
            self.ser = None

        # Main layout
        layout = BoxLayout(orientation='vertical', padding=10, spacing=10)

        # Create a label to display status
        self.response_label = Label(text="arduino will tell you things over here", size_hint=(1, 1), font_size='22sp')

        # Create text input for setting temperature
        self.temperature_input = TextInput(hint_text="enter desired temperature and press ENTER", multiline=False, size_hint=(1, 0.2), background_color=(0, .5, .5), font_size='22sp', halign = 'center')
        self.temperature_input.bind(on_text_validate=self.set_temperature)  # Bind 'Enter' key to set_temperature

        # Create a button to send the temperature command
        #set_temp_button = Button(text="set temperature", size_hint=(1, 0.2))
        #set_temp_button.bind(on_release=self.set_temperature)

        # Add widgets to the layout
        layout.add_widget(Label(text="temperature control: max 100", size_hint=(1, 0.3), color = (.6, 0, .8), font_size='22sp'))
        layout.add_widget(self.temperature_input)
        #layout.add_widget(set_temp_button)
        layout.add_widget(self.response_label)

        return layout

    def set_temperature(self, instance):
        # Get the user input temperature
        temperature = self.temperature_input.text

        # Check if input is a valid number
        if temperature.replace('.', '', 1).isdigit():
            # Send the "SET TEMP <value>" command to Arduino
            command = f"SET TEMP {temperature}"
            self.send_command(command)
        else:
            self.response_label.text = "invalid input"

    def send_command(self, command):
        if self.ser and self.ser.is_open:
            try:
                self.ser.reset_input_buffer()
                self.ser.write((command + '\n').encode('utf-8'))
                print(f"sent command: {command}")
                #self.response_label.text = f"sent command: {command}"

                time.sleep(2)

                # Read Arduino's response
                arduino_responses = []
                while self.ser.in_waiting > 0:
                    response = self.ser.readline().decode('utf-8').strip()
                    if response == "status: 1":
                        self.response_label.text = "heating patiently"
                    
                    elif response:
                        arduino_responses.append(response)

                # Update the label with Arduino's response
                if arduino_responses:
                    self.response_label.text = f"{arduino_responses[-1]}"
                else:
                    self.response_label.text = "no response from arduino"

            except serial.SerialException as e:
                print(f"error sending command: {e}")
                self.response_label.text = "error sending command"
        else:
            print("serial connection is not available.")
            self.response_label.text = "serial connection is not available"

    def on_stop(self):
        # Close serial communication on app close
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("serial port closed.")
            self.response_label.text = "serial port closed"

if __name__ == '__main__':
    TemperatureControlApp().run()