import unittest
import datetime
import json
from unittest.mock import MagicMock

# Mocking the SX127xLoRaSimulator class and global variables to isolate test
class MockLoRa:
    def __init__(self):
        self.sent_packets = []
    def send_packet(self, payload):
        self.sent_packets.append(payload)

class TestGatewaySchedule(unittest.TestCase):
    def test_schedule_trigger(self):
        # Setup test variables
        schedules_list = [{"time": "08:00", "portion": 2}]
        feed_mode = "Auto"
        last_triggered_time = ""
        lora = MockLoRa()
        
        # Scenario 1: Clock is at 07:59 (should NOT trigger)
        now = datetime.datetime(2026, 6, 29, 7, 59, 0)
        current_time_str = now.strftime("%H:%M")
        current_date_hour_str = now.strftime("%Y-%m-%d %H:%M")
        
        triggered = False
        for schedule in schedules_list:
            if schedule.get("time") == current_time_str:
                if last_triggered_time != current_date_hour_str:
                    triggered = True
                    lora.send_packet(json.dumps({"type": "control", "feed": schedule.get("portion")}))
                    last_triggered_time = current_date_hour_str
        
        self.assertFalse(triggered)
        self.assertEqual(len(lora.sent_packets), 0)
        
        # Scenario 2: Clock is at 08:00 (should trigger exactly once)
        now = datetime.datetime(2026, 6, 29, 8, 0, 0)
        current_time_str = now.strftime("%H:%M")
        current_date_hour_str = now.strftime("%Y-%m-%d %H:%M")
        
        triggered = False
        for schedule in schedules_list:
            if schedule.get("time") == current_time_str:
                if last_triggered_time != current_date_hour_str:
                    triggered = True
                    lora.send_packet(json.dumps({"type": "control", "feed": schedule.get("portion")}))
                    last_triggered_time = current_date_hour_str
                    
        self.assertTrue(triggered)
        self.assertEqual(len(lora.sent_packets), 1)
        
        # Parse payload
        data = json.loads(lora.sent_packets[0])
        self.assertEqual(data["type"], "control")
        self.assertEqual(data["feed"], 2)
        
        # Scenario 3: Checking again in the same minute (should NOT trigger again)
        # last_triggered_time is now "2026-06-29 08:00"
        triggered = False
        for schedule in schedules_list:
            if schedule.get("time") == current_time_str:
                if last_triggered_time != current_date_hour_str:
                    triggered = True
                    lora.send_packet(json.dumps({"type": "control", "feed": schedule.get("portion")}))
                    last_triggered_time = current_date_hour_str
                    
        self.assertFalse(triggered)
        self.assertEqual(len(lora.sent_packets), 1) # Still 1

if __name__ == '__main__':
    unittest.main()
