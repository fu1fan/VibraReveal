import QtQuick 2.15
import QtQuick.Controls 2.15
import QtTest 1.2

Item {
    width: 1024
    height: 600
    Button { id: action; text: "测振"; checkable: true }
    TestCase {
        name: "QtEnvironment"
        when: windowShown
        function test_controlInput() {
            mouseClick(action)
            compare(action.checked, true)
        }
    }
}
