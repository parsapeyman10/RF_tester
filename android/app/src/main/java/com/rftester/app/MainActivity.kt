package com.rftester.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BarChart
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Insights
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.rftester.app.ui.RfViewModel
import com.rftester.app.ui.screens.analytics.AnalyticsScreen
import com.rftester.app.ui.screens.charts.ChartsScreen
import com.rftester.app.ui.screens.dashboard.DashboardScreen
import com.rftester.app.ui.screens.history.HistoryScreen
import com.rftester.app.ui.screens.settings.SettingsScreen
import com.rftester.app.ui.theme.RFTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            val viewModel: RfViewModel = viewModel(factory = RfViewModel.Factory)
            val dark by viewModel.darkMode.collectAsState()
            RFTheme(dark = dark) {
                AppRoot(viewModel)
            }
        }
    }
}

private data class Tab(
    val route: String,
    val label: String,
    val icon: ImageVector
)

private val tabs = listOf(
    Tab("dashboard", "داشبورد", Icons.Default.Home),
    Tab("charts", "نمودارها", Icons.Default.BarChart),
    Tab("analytics", "تحلیل", Icons.Default.Insights),
    Tab("history", "تاریخچه", Icons.Default.History)
)

@Composable
fun AppRoot(vm: RfViewModel) {
    val nav = rememberNavController()
    var selected by remember { mutableIntStateOf(0) }
    var inSettings by remember { mutableIntStateOf(0) } // 0 = خارج از تنظیمات

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        bottomBar = {
            if (inSettings == 0) {
                NavigationBar(
                    containerColor = MaterialTheme.colorScheme.surface
                ) {
                    tabs.forEachIndexed { i, tab ->
                        NavigationBarItem(
                            selected = selected == i,
                            onClick = {
                                selected = i
                                nav.navigate(tab.route) {
                                    popUpTo("dashboard") { saveState = true }
                                    launchSingleTop = true
                                    restoreState = true
                                }
                            },
                            icon = { Icon(tab.icon, contentDescription = tab.label) },
                            label = { Text(tab.label) },
                            colors = NavigationBarItemDefaults.colors(
                                selectedIconColor = MaterialTheme.colorScheme.primary,
                                selectedTextColor = MaterialTheme.colorScheme.primary,
                                indicatorColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.14f)
                            )
                        )
                    }
                }
            }
        }
    ) { pad ->
        Box(Modifier.fillMaxSize().padding(pad)) {
            NavHost(
                navController = nav,
                startDestination = "dashboard"
            ) {
                composable("dashboard") {
                    inSettings = 0
                    DashboardScreen(vm, onOpenSettings = {
                        inSettings = 1
                        nav.navigate("settings")
                    })
                }
                composable("charts") {
                    inSettings = 0
                    ChartsScreen(vm)
                }
                composable("analytics") {
                    inSettings = 0
                    AnalyticsScreen(vm)
                }
                composable("history") {
                    inSettings = 0
                    HistoryScreen(vm)
                }
                composable("settings") {
                    inSettings = 1
                    SettingsScreen(vm, onBack = {
                        inSettings = 0
                        nav.popBackStack()
                    })
                }
            }
        }
    }
}
